#include "snmp_manager.h"

#include <stdio.h>
#include <string.h>

#include "audit_log.h"
#include "cJSON.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_manager.h"
#include "lwip/sockets.h"
#include "network_manager.h"
#include "ota_manager.h"
#include "sensor_manager.h"
#include "wireguard_manager.h"

static const char* TAG = "snmp_manager";

#define SNMP_PORT 161
#define SNMP_CONFIG_FILE "/storage/snmp.json"
#define COMMUNITY_CAP 32

// Getrennte Lese-/Schreib-Community (Standard-SNMP-Konvention,
// ro/rwcommunity) - siehe docs/entscheidungen.md "SNMP-Agent: SET fuer
// Power/Reset-Taste". SNMP kennt keine Benutzeranmeldung wie Web/USB
// (keine Passwort-Rueckbestaetigung fuer Verwalter moeglich), deshalb
// bewusst ein eigenes, staerker zu schuetzendes Schreib-Geheimnis statt
// den Lese-Community fuer SET mitzubenutzen.
static char s_community[COMMUNITY_CAP] = "public";
static char s_rw_community[COMMUNITY_CAP] = "private";

// =============================================================================
// Minimaler BER/ASN.1-Encoder/Decoder - bewusst kein allgemeiner
// MIB-Compiler oder lwIP-eigenes SNMP-APPS-Modul (dessen private-MIB-
// C-API ist sehr umstaendlich, siehe docs/entscheidungen.md), sondern
// direkt auf den genau benoetigten Nachrichtenteilen zugeschnitten -
// analog dem Ansatz der Sensormeter-Familie (dort ebenfalls von Hand
// kodiert, nur dort in C++/Arduino mit einer dritten Bibliothek). Fuer
// eine kleine, feste Skalar-OID-Tabelle ist das der pragmatischste Weg
// in reinem ESP-IDF.
// =============================================================================

#define TAG_INTEGER 0x02
#define TAG_OCTET_STRING 0x04
#define TAG_NULL 0x05
#define TAG_OID 0x06
#define TAG_SEQUENCE 0x30
#define TAG_TIMETICKS 0x43
#define TAG_GAUGE32 0x42
#define TAG_GET_REQUEST 0xA0
#define TAG_GETNEXT_REQUEST 0xA1
#define TAG_GET_RESPONSE 0xA2
#define TAG_SET_REQUEST 0xA3

// RFC1157-Fehlercodes (nur diese - kein SNMPv2c-Exception-Value-Zoo).
#define ERR_NO_ERROR 0
#define ERR_NO_SUCH_NAME 2
#define ERR_BAD_VALUE 3
#define ERR_READ_ONLY 4
#define ERR_GEN_ERR 5

typedef struct {
  const uint8_t* data;
  size_t len;
  size_t pos;
} ber_reader_t;

static bool ber_read_tlv(ber_reader_t* r, uint8_t* tag, const uint8_t** content, size_t* content_len) {
  if (r->pos >= r->len) return false;
  *tag = r->data[r->pos++];
  if (r->pos >= r->len) return false;
  uint8_t first_len = r->data[r->pos++];
  size_t clen;
  if (first_len < 0x80) {
    clen = first_len;
  } else {
    uint8_t nbytes = first_len & 0x7F;
    if (nbytes == 0 || nbytes > 4 || r->pos + nbytes > r->len) return false;
    clen = 0;
    for (uint8_t i = 0; i < nbytes; i++) clen = (clen << 8) | r->data[r->pos++];
  }
  if (r->pos + clen > r->len) return false;
  *content = &r->data[r->pos];
  *content_len = clen;
  r->pos += clen;
  return true;
}

static int64_t ber_decode_int(const uint8_t* content, size_t len) {
  int64_t v = (len > 0 && (content[0] & 0x80)) ? -1 : 0;  // Vorzeichen-Ausdehnung
  for (size_t i = 0; i < len; i++) v = (v << 8) | content[i];
  return v;
}

// Dekodiert eine OBJECT IDENTIFIER-Nutzlast nach out (bis zu out_cap
// Komponenten), liefert die Anzahl.
static uint8_t ber_decode_oid(const uint8_t* content, size_t len, uint32_t* out, uint8_t out_cap) {
  if (len == 0 || out_cap < 2) return 0;
  uint8_t n = 0;
  out[n++] = content[0] / 40;
  out[n++] = content[0] % 40;
  uint32_t v = 0;
  for (size_t i = 1; i < len && n < out_cap; i++) {
    v = (v << 7) | (content[i] & 0x7F);
    if (!(content[i] & 0x80)) {
      out[n++] = v;
      v = 0;
    }
  }
  return n;
}

static size_t ber_write_tlv(uint8_t* out, uint8_t tag, const uint8_t* content, size_t content_len) {
  size_t off = 0;
  out[off++] = tag;
  if (content_len < 0x80) {
    out[off++] = (uint8_t)content_len;
  } else if (content_len < 0x100) {
    out[off++] = 0x81;
    out[off++] = (uint8_t)content_len;
  } else {
    out[off++] = 0x82;
    out[off++] = (uint8_t)(content_len >> 8);
    out[off++] = (uint8_t)(content_len & 0xFF);
  }
  memcpy(out + off, content, content_len);
  off += content_len;
  return off;
}

// Minimalcodierung eines vorzeichenbehafteten INTEGER (Zweierkomplement,
// so wenig Bytes wie moeglich ohne die Vorzeichenbedeutung zu aendern).
static size_t ber_encode_int_content(int32_t v, uint8_t* out) {
  uint8_t buf[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  int start = 0;
  while (start < 3) {
    if (buf[start] == 0x00 && !(buf[start + 1] & 0x80)) {
      start++;
    } else if (buf[start] == 0xFF && (buf[start + 1] & 0x80)) {
      start++;
    } else {
      break;
    }
  }
  memcpy(out, buf + start, 4 - start);
  return 4 - start;
}

// Minimalcodierung eines vorzeichenlosen 32-Bit-Werts (TimeTicks/Gauge32) -
// braucht ein fuehrendes 0x00, falls das hoechstwertige Byte sonst als
// negativ gelesen wuerde.
static size_t ber_encode_uint_content(uint32_t v, uint8_t* out) {
  uint8_t buf[5] = {0x00, (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  int start = 0;
  while (start < 4 && buf[start] == 0x00 && !(buf[start + 1] & 0x80)) start++;
  memcpy(out, buf + start, 5 - start);
  return 5 - start;
}

static size_t ber_encode_oid_content(const uint32_t* oid, uint8_t len, uint8_t* out) {
  size_t off = 0;
  out[off++] = (uint8_t)(oid[0] * 40 + oid[1]);
  for (uint8_t i = 2; i < len; i++) {
    uint32_t v = oid[i];
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    v >>= 7;
    while (v > 0) {
      tmp[n++] = v & 0x7F;
      v >>= 7;
    }
    for (int j = n - 1; j >= 0; j--) out[off++] = tmp[j] | (j == 0 ? 0x00 : 0x80);
  }
  return off;
}

// Komponentenweiser lexikografischer Vergleich zweier vollstaendiger OIDs
// (Standard-SNMP-Ordnungsregel: bei gleichem Praefix ist die kuerzere OID
// "kleiner"). Ergebnis wie memcmp.
static int oid_compare(const uint32_t* a, uint8_t a_len, const uint32_t* b, uint8_t b_len) {
  uint8_t n = a_len < b_len ? a_len : b_len;
  for (uint8_t i = 0; i < n; i++) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  if (a_len < b_len) return -1;
  if (a_len > b_len) return 1;
  return 0;
}

// =============================================================================
// Private Enterprise-MIB - 1.3.6.1.4.1.99999.10.<Objekt>.0 (Skalare,
// deshalb immer Instanz ".0"). 99999 ist dieselbe (unregistrierte,
// frei erfundene) "Haushalts"-Enterprise-Nummer wie bei der
// Sensormeter-Familie, Zweig .10 ist neu und ESP-BMC vorbehalten
// (Sensormeter belegt .1 bis .5 direkt unter .99999). Neue Objekte
// werden immer angehaengt (hoechste freie Nummer), bestehende OIDs
// werden nie umnummeriert - siehe docs/entscheidungen.md.
// =============================================================================

static const uint32_t OID_SYS_NAME[] = {1, 3, 6, 1, 4, 1, 99999, 10, 1, 0};
static const uint32_t OID_UPTIME[] = {1, 3, 6, 1, 4, 1, 99999, 10, 2, 0};
static const uint32_t OID_WLAN_IP[] = {1, 3, 6, 1, 4, 1, 99999, 10, 3, 0};
static const uint32_t OID_WLAN_SSID[] = {1, 3, 6, 1, 4, 1, 99999, 10, 4, 0};
static const uint32_t OID_VPN_UP[] = {1, 3, 6, 1, 4, 1, 99999, 10, 5, 0};
static const uint32_t OID_VPN_IP[] = {1, 3, 6, 1, 4, 1, 99999, 10, 6, 0};
static const uint32_t OID_NTC_TEMP10[] = {1, 3, 6, 1, 4, 1, 99999, 10, 7, 0};
static const uint32_t OID_DHT_TEMP10[] = {1, 3, 6, 1, 4, 1, 99999, 10, 8, 0};
static const uint32_t OID_DHT_HUM10[] = {1, 3, 6, 1, 4, 1, 99999, 10, 9, 0};
static const uint32_t OID_POWER_LED[] = {1, 3, 6, 1, 4, 1, 99999, 10, 10, 0};
static const uint32_t OID_HDD_LED[] = {1, 3, 6, 1, 4, 1, 99999, 10, 11, 0};
static const uint32_t OID_FREE_HEAP[] = {1, 3, 6, 1, 4, 1, 99999, 10, 12, 0};
static const uint32_t OID_WLAN_STATIC[] = {1, 3, 6, 1, 4, 1, 99999, 10, 13, 0};
static const uint32_t OID_SYS_TYPE[] = {1, 3, 6, 1, 4, 1, 99999, 10, 14, 0};
static const uint32_t OID_POWER_KEY[] = {1, 3, 6, 1, 4, 1, 99999, 10, 15, 0};
static const uint32_t OID_RESET_KEY[] = {1, 3, 6, 1, 4, 1, 99999, 10, 16, 0};
static const uint32_t OID_FIRMWARE_VERSION[] = {1, 3, 6, 1, 4, 1, 99999, 10, 17, 0};

typedef enum { VT_STR, VT_INT, VT_TICKS, VT_GAUGE } val_type_t;

static void get_sys_name(char* out, size_t n) { config_manager_get_device_name(out, n); }
static void get_sys_type(char* out, size_t n) { snprintf(out, n, "%s", config_manager_get_device_type()); }
static void get_uptime_ticks(uint32_t* out) { *out = (uint32_t)(esp_timer_get_time() / 10000); }  // Centisekunden
static void get_wlan_ip(char* out, size_t n) {
  if (!network_manager_get_ip_string(out, n)) snprintf(out, n, "0.0.0.0");
}
static void get_wlan_ssid(char* out, size_t n) { network_manager_get_ssid(out, n); }
static void get_vpn_up(int32_t* out) { *out = wireguard_manager_is_up() ? 1 : 0; }
static void get_vpn_ip(char* out, size_t n) { wireguard_manager_get_local_address(out, n); }
static void get_ntc_temp10(int32_t* out) {
  float t;
  *out = sensor_manager_get_ntc_temp_c(&t) ? (int32_t)(t * 10) : -32768;
}
static void get_dht_temp10(int32_t* out) {
  float t;
  *out = sensor_manager_get_dht_temp_c(&t) ? (int32_t)(t * 10) : -32768;
}
static void get_dht_hum10(int32_t* out) {
  float h;
  *out = sensor_manager_get_dht_humidity_pct(&h) ? (int32_t)(h * 10) : -32768;
}
static void get_power_led(int32_t* out) { *out = gpio_manager_read_power_led() ? 1 : 0; }
static void get_hdd_led(int32_t* out) { *out = gpio_manager_hdd_led_active_recently() ? 1 : 0; }
static void get_free_heap(uint32_t* out) { *out = (uint32_t)esp_get_free_heap_size(); }
static void get_wlan_static(int32_t* out) { *out = network_manager_is_static_ip() ? 1 : 0; }
static void get_firmware_version(char* out, size_t n) { snprintf(out, n, "%s", ota_manager_get_version()); }

// GET auf power-/resetKey liefert den aktuellen Weiterleitungs-Zustand
// (1 = gerade aktiv, egal ob physisch oder per SNMP/Web/USB ausgeloest) -
// nuetzlich um einen laufenden Tastendruck von aussen zu erkennen.
static void get_power_key(int32_t* out) { *out = gpio_manager_power_taste_weitergeleitet() ? 1 : 0; }
static void get_reset_key(int32_t* out) { *out = gpio_manager_reset_taste_weitergeleitet() ? 1 : 0; }

// SET auf power-/resetKey loest einen Tastendruck aus - identische
// GPIO-Logik wie die Taster-Steuerung im Web/USB
// (gpio_manager_trigger_power/_reset respektieren selbst den
// Tastschutz). powerKey: 1=kurzer Druck, 2=langer Druck (erzwungenes
// Abschalten). resetKey: nur 1=kurzer Druck. Rueckgabe false wird vom
// Aufrufer als badValue/Tastschutz-Ablehnung beantwortet.
static const char* s_set_source_ip;  // waehrend handle_packet() gueltig, fuers Audit-Log

static bool set_power_key(int32_t value) {
  bool hold;
  if (value == 1) {
    hold = false;
  } else if (value == 2) {
    hold = true;
  } else {
    return false;
  }
  bool ok = gpio_manager_trigger_power(hold);
  char event[96];
  snprintf(event, sizeof(event), "Taster \"Power (%s)\" ausgeloest von %s (SNMP)%s", hold ? "lang" : "kurz",
           s_set_source_ip ? s_set_source_ip : "?", ok ? "" : " - durch Tastschutz blockiert");
  audit_log_add(event);
  ESP_LOGI(TAG, "%s", event);
  return ok;
}

static bool set_reset_key(int32_t value) {
  if (value != 1) return false;
  bool ok = gpio_manager_trigger_reset();
  char event[96];
  snprintf(event, sizeof(event), "Taster \"Reset\" ausgeloest von %s (SNMP)%s",
           s_set_source_ip ? s_set_source_ip : "?", ok ? "" : " - durch Tastschutz blockiert");
  audit_log_add(event);
  ESP_LOGI(TAG, "%s", event);
  return ok;
}

typedef struct {
  const uint32_t* oid;
  uint8_t oid_len;
  val_type_t type;
  void (*get_int)(int32_t*);
  void (*get_uint)(uint32_t*);
  void (*get_str)(char*, size_t);
  bool (*set_int)(int32_t value);  // nur bei schreibbaren Objekten gesetzt
} oid_entry_t;

// MUSS aufsteigend nach OID sortiert bleiben - GETNEXT verlaesst sich
// beim linearen Scan auf diese Reihenfolge.
static const oid_entry_t s_oids[] = {
    {OID_SYS_NAME, 10, VT_STR, NULL, NULL, get_sys_name, NULL},
    {OID_UPTIME, 10, VT_TICKS, NULL, get_uptime_ticks, NULL, NULL},
    {OID_WLAN_IP, 10, VT_STR, NULL, NULL, get_wlan_ip, NULL},
    {OID_WLAN_SSID, 10, VT_STR, NULL, NULL, get_wlan_ssid, NULL},
    {OID_VPN_UP, 10, VT_INT, get_vpn_up, NULL, NULL, NULL},
    {OID_VPN_IP, 10, VT_STR, NULL, NULL, get_vpn_ip, NULL},
    {OID_NTC_TEMP10, 10, VT_INT, get_ntc_temp10, NULL, NULL, NULL},
    {OID_DHT_TEMP10, 10, VT_INT, get_dht_temp10, NULL, NULL, NULL},
    {OID_DHT_HUM10, 10, VT_INT, get_dht_hum10, NULL, NULL, NULL},
    {OID_POWER_LED, 10, VT_INT, get_power_led, NULL, NULL, NULL},
    {OID_HDD_LED, 10, VT_INT, get_hdd_led, NULL, NULL, NULL},
    {OID_FREE_HEAP, 10, VT_GAUGE, NULL, get_free_heap, NULL, NULL},
    {OID_WLAN_STATIC, 10, VT_INT, get_wlan_static, NULL, NULL, NULL},
    {OID_SYS_TYPE, 10, VT_STR, NULL, NULL, get_sys_type, NULL},
    {OID_POWER_KEY, 10, VT_INT, get_power_key, NULL, NULL, set_power_key},
    {OID_RESET_KEY, 10, VT_INT, get_reset_key, NULL, NULL, set_reset_key},
    {OID_FIRMWARE_VERSION, 10, VT_STR, NULL, NULL, get_firmware_version, NULL},
};
#define OID_COUNT (sizeof(s_oids) / sizeof(s_oids[0]))

// Encodiert den Wert eines Tabelleneintrags als TLV nach out, liefert die
// Byteanzahl.
static size_t encode_value(const oid_entry_t* e, uint8_t* out) {
  uint8_t content[64];
  size_t clen;
  switch (e->type) {
    case VT_STR: {
      char buf[64];
      e->get_str(buf, sizeof(buf));
      clen = strlen(buf);
      memcpy(content, buf, clen);
      return ber_write_tlv(out, TAG_OCTET_STRING, content, clen);
    }
    case VT_INT: {
      int32_t v;
      e->get_int(&v);
      clen = ber_encode_int_content(v, content);
      return ber_write_tlv(out, TAG_INTEGER, content, clen);
    }
    case VT_TICKS: {
      uint32_t v;
      e->get_uint(&v);
      clen = ber_encode_uint_content(v, content);
      return ber_write_tlv(out, TAG_TIMETICKS, content, clen);
    }
    case VT_GAUGE:
    default: {
      uint32_t v;
      e->get_uint(&v);
      clen = ber_encode_uint_content(v, content);
      return ber_write_tlv(out, TAG_GAUGE32, content, clen);
    }
  }
}

// Sucht den exakten Treffer (GET/SET) bzw. den naechstgroesseren Eintrag
// (GETNEXT) fuer eine angefragte OID. Liefert NULL, wenn nichts passt.
static const oid_entry_t* lookup(const uint32_t* req_oid, uint8_t req_len, bool next) {
  if (!next) {
    for (size_t i = 0; i < OID_COUNT; i++) {
      if (oid_compare(s_oids[i].oid, s_oids[i].oid_len, req_oid, req_len) == 0) return &s_oids[i];
    }
    return NULL;
  }
  for (size_t i = 0; i < OID_COUNT; i++) {
    if (oid_compare(s_oids[i].oid, s_oids[i].oid_len, req_oid, req_len) > 0) return &s_oids[i];
  }
  return NULL;
}

// Baut eine komplette GetResponse-Nachricht (Version+Community bleiben wie
// angefragt). error_status/error_index folgen RFC1157 (0=noError,
// 2=noSuchName, 3=badValue, 4=readOnly, 5=genErr). Bei Erfolg enthaelt
// die Varbind-Liste die tatsaechlich gefundenen OIDs+aktuellen Werte
// (auch nach einem erfolgreichen SET - dann der frisch geltende
// Zustand, kein blosses Echo des Requests), sonst die urspruenglich
// angefragten OIDs mit NULL-Werten (Standardverhalten bei einem Fehler:
// die gesamte PDU meldet den Fehler, keine Teilantworten).
static size_t build_response(uint8_t version, const char* community, int32_t request_id, int32_t error_status,
                              int32_t error_index, const oid_entry_t** found, const uint32_t** orig_oid,
                              const uint8_t* orig_oid_len, int n, uint8_t* out, size_t out_cap) {
  uint8_t varbind_list[1024];
  size_t vl_off = 0;
  for (int i = 0; i < n; i++) {
    uint8_t oid_content[64];
    uint8_t oid_tlv[72];
    uint8_t val_tlv[96];
    size_t oid_clen, oid_tlen, val_tlen;
    if (found[i]) {
      oid_clen = ber_encode_oid_content(found[i]->oid, found[i]->oid_len, oid_content);
      oid_tlen = ber_write_tlv(oid_tlv, TAG_OID, oid_content, oid_clen);
      val_tlen = encode_value(found[i], val_tlv);
    } else {
      oid_clen = ber_encode_oid_content(orig_oid[i], orig_oid_len[i], oid_content);
      oid_tlen = ber_write_tlv(oid_tlv, TAG_OID, oid_content, oid_clen);
      val_tlen = ber_write_tlv(val_tlv, TAG_NULL, NULL, 0);
    }
    uint8_t varbind_content[200];
    size_t vc_off = 0;
    memcpy(varbind_content + vc_off, oid_tlv, oid_tlen);
    vc_off += oid_tlen;
    memcpy(varbind_content + vc_off, val_tlv, val_tlen);
    vc_off += val_tlen;
    vl_off += ber_write_tlv(varbind_list + vl_off, TAG_SEQUENCE, varbind_content, vc_off);
  }

  uint8_t varbind_list_tlv[1040];
  size_t vlt_len = ber_write_tlv(varbind_list_tlv, TAG_SEQUENCE, varbind_list, vl_off);

  uint8_t int_buf[4];
  uint8_t pdu_content[1100];
  size_t pc_off = 0;
  size_t ilen = ber_encode_int_content(request_id, int_buf);
  pc_off += ber_write_tlv(pdu_content + pc_off, TAG_INTEGER, int_buf, ilen);
  ilen = ber_encode_int_content(error_status, int_buf);
  pc_off += ber_write_tlv(pdu_content + pc_off, TAG_INTEGER, int_buf, ilen);
  ilen = ber_encode_int_content(error_index, int_buf);
  pc_off += ber_write_tlv(pdu_content + pc_off, TAG_INTEGER, int_buf, ilen);
  memcpy(pdu_content + pc_off, varbind_list_tlv, vlt_len);
  pc_off += vlt_len;

  uint8_t pdu_tlv[1120];
  size_t pdu_len = ber_write_tlv(pdu_tlv, TAG_GET_RESPONSE, pdu_content, pc_off);

  uint8_t msg_content[1200];
  size_t mc_off = 0;
  ilen = ber_encode_int_content(version, int_buf);
  mc_off += ber_write_tlv(msg_content + mc_off, TAG_INTEGER, int_buf, ilen);
  mc_off += ber_write_tlv(msg_content + mc_off, TAG_OCTET_STRING, (const uint8_t*)community, strlen(community));
  memcpy(msg_content + mc_off, pdu_tlv, pdu_len);
  mc_off += pdu_len;

  if (mc_off + 4 > out_cap) return 0;  // sollte bei unserer kleinen Tabelle nie passieren
  return ber_write_tlv(out, TAG_SEQUENCE, msg_content, mc_off);
}

#define MAX_VARBINDS 10

static void handle_packet(int sock, const uint8_t* buf, size_t len, const struct sockaddr_in* from,
                           socklen_t from_len) {
  ber_reader_t r = {.data = buf, .len = len, .pos = 0};
  uint8_t tag;
  const uint8_t *content, *msg_body;
  size_t clen, msg_body_len;

  if (!ber_read_tlv(&r, &tag, &msg_body, &msg_body_len) || tag != TAG_SEQUENCE) return;
  ber_reader_t mr = {.data = msg_body, .len = msg_body_len, .pos = 0};

  if (!ber_read_tlv(&mr, &tag, &content, &clen) || tag != TAG_INTEGER) return;
  int64_t version = ber_decode_int(content, clen);
  if (version != 0 && version != 1) return;  // nur SNMPv1(0)/v2c(1)

  if (!ber_read_tlv(&mr, &tag, &content, &clen) || tag != TAG_OCTET_STRING) return;
  char community[COMMUNITY_CAP];
  size_t community_len = clen < sizeof(community) - 1 ? clen : sizeof(community) - 1;
  memcpy(community, content, community_len);
  community[community_len] = '\0';

  bool ro_match = strcmp(community, s_community) == 0;
  bool rw_match = strcmp(community, s_rw_community) == 0;
  if (!ro_match && !rw_match) return;  // unbekannte Community: stillschweigend ignorieren

  const uint8_t* pdu_body;
  size_t pdu_body_len;
  if (!ber_read_tlv(&mr, &tag, &pdu_body, &pdu_body_len)) return;
  bool is_get = (tag == TAG_GET_REQUEST);
  bool is_getnext = (tag == TAG_GETNEXT_REQUEST);
  bool is_set = (tag == TAG_SET_REQUEST);

  ber_reader_t pr = {.data = pdu_body, .len = pdu_body_len, .pos = 0};
  if (!ber_read_tlv(&pr, &tag, &content, &clen) || tag != TAG_INTEGER) return;
  int32_t request_id = (int32_t)ber_decode_int(content, clen);
  if (!ber_read_tlv(&pr, &tag, &content, &clen) || tag != TAG_INTEGER) return;  // error-status (im Request 0)
  if (!ber_read_tlv(&pr, &tag, &content, &clen) || tag != TAG_INTEGER) return;  // error-index (im Request 0)
  if (!ber_read_tlv(&pr, &tag, &content, &clen) || tag != TAG_SEQUENCE) return;

  ber_reader_t vr = {.data = content, .len = clen, .pos = 0};
  const oid_entry_t* found[MAX_VARBINDS] = {0};
  const uint32_t* orig_oid[MAX_VARBINDS];
  uint8_t orig_oid_len[MAX_VARBINDS];
  int n = 0;
  int32_t error_status = ERR_NO_ERROR, error_index = 0;

  if (!is_get && !is_getnext && !is_set) {
    // GetBulk/unbekannt - nicht unterstuetzt.
    uint8_t resp[96];
    size_t rlen = build_response((uint8_t)version, community, request_id, ERR_GEN_ERR, 0, found, orig_oid,
                                  orig_oid_len, 0, resp, sizeof(resp));
    if (rlen > 0) sendto(sock, resp, rlen, 0, (const struct sockaddr*)from, from_len);
    return;
  }

  if (is_set && !rw_match) {
    // Community kennt der Agent, aber sie ist nicht die Schreib-Community -
    // klare Fehlerantwort statt stillem Verwerfen (kein zusaetzliches
    // Informationsleck, die Community war ja bereits als "bekannt"
    // bestaetigt).
    uint8_t resp[96];
    size_t rlen = build_response((uint8_t)version, community, request_id, ERR_GEN_ERR, 1, found, orig_oid,
                                  orig_oid_len, 0, resp, sizeof(resp));
    if (rlen > 0) sendto(sock, resp, rlen, 0, (const struct sockaddr*)from, from_len);
    return;
  }

  static uint32_t req_oid_buf[MAX_VARBINDS][16];
  int32_t set_values[MAX_VARBINDS];

  // Muss VOR der Varbind-Schleife gesetzt sein - set_power_key()/
  // set_reset_key() lesen das fuer den Audit-Log-Eintrag direkt beim
  // Ausloesen (innerhalb der Schleife unten), nicht erst danach.
  if (is_set) {
    s_set_source_ip = inet_ntoa(from->sin_addr);
  }

  while (n < MAX_VARBINDS) {
    const uint8_t* vb_body;
    size_t vb_body_len;
    if (!ber_read_tlv(&vr, &tag, &vb_body, &vb_body_len) || tag != TAG_SEQUENCE) break;
    ber_reader_t vbr = {.data = vb_body, .len = vb_body_len, .pos = 0};
    if (!ber_read_tlv(&vbr, &tag, &content, &clen) || tag != TAG_OID) break;

    uint8_t req_len = ber_decode_oid(content, clen, req_oid_buf[n], 16);
    orig_oid[n] = req_oid_buf[n];
    orig_oid_len[n] = req_len;

    if (is_set) {
      // Bei SET folgt nach der OID der tatsaechliche Zielwert (bei uns
      // immer INTEGER, da powerKey/resetKey die einzigen schreibbaren
      // Objekte sind).
      if (!ber_read_tlv(&vbr, &tag, &content, &clen) || tag != TAG_INTEGER) break;
      set_values[n] = (int32_t)ber_decode_int(content, clen);
    }

    const oid_entry_t* e = lookup(req_oid_buf[n], req_len, is_getnext);

    if (is_set) {
      if (!e || !e->set_int) {
        if (error_status == ERR_NO_ERROR) {
          error_status = e ? ERR_READ_ONLY : ERR_NO_SUCH_NAME;
          error_index = n + 1;
        }
        found[n] = NULL;
      } else if (!e->set_int(set_values[n])) {
        if (error_status == ERR_NO_ERROR) {
          error_status = ERR_BAD_VALUE;
          error_index = n + 1;
        }
        found[n] = NULL;
      } else {
        found[n] = e;  // Erfolg - Antwort spiegelt den frischen Ist-Zustand
      }
    } else {
      if (!e && error_status == ERR_NO_ERROR) {
        error_status = ERR_NO_SUCH_NAME;
        error_index = n + 1;
      }
      found[n] = e;
    }
    n++;
  }
  if (n == 0) return;

  if (error_status != ERR_NO_ERROR) {
    // Bei einem Fehler meldet die gesamte PDU den Fehler, keine
    // Teilerfolge - bereits ausgefuehrte SETs in dieser PDU (falls vor
    // dem fehlerhaften Varbind welche erfolgreich waren) bleiben zwar
    // wirksam, da wir keine echte Transaktions-Semantik ueber mehrere
    // physische Aktionen hinweg umsetzen koennen (ein Tastendruck laesst
    // sich nicht "zuruecknehmen") - bei genau einem Varbind pro Request
    // (der ueblichen Zabbix-Nutzung) ist das ohnehin nicht beobachtbar.
    for (int i = 0; i < n; i++) found[i] = NULL;
  }

  uint8_t resp[1400];
  size_t rlen = build_response((uint8_t)version, community, request_id, error_status, error_index, found, orig_oid,
                                orig_oid_len, n, resp, sizeof(resp));
  if (rlen > 0) sendto(sock, resp, rlen, 0, (const struct sockaddr*)from, from_len);
  s_set_source_ip = NULL;
}

static void snmp_task(void* arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Socket konnte nicht erstellt werden");
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(SNMP_PORT);
  if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "Bind auf Port %d fehlgeschlagen", SNMP_PORT);
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "SnmpManager gestartet (Port %d)", SNMP_PORT);

  uint8_t buf[512];
  for (;;) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &from_len);
    if (n > 0) {
      handle_packet(sock, buf, (size_t)n, &from, from_len);
    }
  }
}

// ---------------------------------------------------------------------
// Community-Persistenz (analog network_manager/wireguard_manager: JSON
// auf der storage-Partition, Default-Werte falls keine Datei existiert).
// ---------------------------------------------------------------------

static void save_communities(void) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "community", s_community);
  cJSON_AddStringToObject(root, "rw_community", s_rw_community);
  char* text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) return;

  FILE* f = fopen(SNMP_CONFIG_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  }
  cJSON_free(text);
}

static void load_communities(void) {
  FILE* f = fopen(SNMP_CONFIG_FILE, "r");
  if (!f) return;

  char buf[128];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* root = cJSON_Parse(buf);
  if (!root) return;
  cJSON* item = cJSON_GetObjectItem(root, "community");
  if (cJSON_IsString(item)) strncpy(s_community, item->valuestring, sizeof(s_community) - 1);
  item = cJSON_GetObjectItem(root, "rw_community");
  if (cJSON_IsString(item)) strncpy(s_rw_community, item->valuestring, sizeof(s_rw_community) - 1);
  cJSON_Delete(root);
}

void snmp_manager_init(void) {
  load_communities();
  // ACHTUNG (2026-07-23, SNMP-Test): 4096 Byte reichten fuer reine
  // GET-Anfragen, aber ein echtes SET (set_power_key(), ueber
  // gpio_manager_trigger_power() + audit_log_add() + die
  // BER-Response-Kodierung obendrauf) hat auf echter Hardware
  // reproduzierbar einen Stack-Overflow ausgeloest ("A stack overflow in
  // task snmp_manager has been detected", siehe docs/entscheidungen.md).
  // Grosszuegig auf 8192 angehoben - gleiche Groessenordnung wie
  // ssh_manager's dedizierter Task, aus demselben Grund (mehrschichtige
  // Aufrufe mit lokalen Formatpuffern).
  xTaskCreate(snmp_task, "snmp_manager", 8192, NULL, 4, NULL);
}

void snmp_manager_get_community(char* out, size_t out_len) {
  strncpy(out, s_community, out_len - 1);
  out[out_len - 1] = '\0';
}

bool snmp_manager_set_community(const char* community) {
  size_t len = strlen(community);
  if (len == 0 || len >= COMMUNITY_CAP) return false;
  strncpy(s_community, community, sizeof(s_community) - 1);
  save_communities();
  return true;
}

void snmp_manager_get_rw_community(char* out, size_t out_len) {
  strncpy(out, s_rw_community, out_len - 1);
  out[out_len - 1] = '\0';
}

bool snmp_manager_set_rw_community(const char* community) {
  size_t len = strlen(community);
  if (len == 0 || len >= COMMUNITY_CAP) return false;
  strncpy(s_rw_community, community, sizeof(s_rw_community) - 1);
  save_communities();
  return true;
}
