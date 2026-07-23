#include "network_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "time_manager.h"

static const char* TAG = "network_manager";
#define WLAN_CONFIG_FILE "/storage/wlan.json"

// Bewaehrtes Reconnect-/Fallback-AP-Muster aus sensormeter-wlan
// (NetworkManager.cpp) - siehe network_manager_tick() fuer die Details.
// Zeitkonstanten 1:1 von dort uebernommen.
#define NETWORK_TICK_INTERVAL_US (5ULL * 1000ULL * 1000ULL)
#define RECONNECT_RETRY_INTERVAL_US (20ULL * 1000ULL * 1000ULL)
// Ist ein WLAN konfiguriert, aber (voruebergehend) nicht erreichbar, wird
// erst nach diesem laengeren Fenster in den Fallback-AP geschaltet - Zeit fuer
// Reconnect-Versuche, bevor eine einmal funktionierende Verbindung aufgegeben
// wird. Ist dagegen GAR KEIN WLAN konfiguriert, gibt es nichts zu verbinden -
// dann sofort (nach WLAN_CHECK_TIMEOUT_NO_CONFIG_US) in den Installer-AP, damit
// die Ersteinrichtung schnell moeglich ist.
#define WLAN_CHECK_TIMEOUT_US (5ULL * 60ULL * 1000000ULL)
#define WLAN_CHECK_TIMEOUT_NO_CONFIG_US (10ULL * 1000000ULL)
// Periodischer Hintergrund-Scan (nur wenn nicht im Normalbetrieb verbunden),
// damit die Netzwerkliste im Webinterface aktuell bleibt.
#define WLAN_SCAN_INTERVAL_US (30ULL * 1000000ULL)
#define MAX_SCAN_CACHE 16

// ACHTUNG (2026-07-23): weicht bewusst von der urspruenglichen Entscheidung
// "Kein AP-Fallback-Abschnitt" in docs/entscheidungen.md ab (dort begruendet
// mit der USB-Ersteinrichtung ueber tools/Setup.ps1). Auf ausdruecklichen
// Wunsch nachtraeglich ergaenzt, siehe dortiger Eintrag "Fallback-Access-
// Point (Nachtrag)" fuer die Gegendarstellung - die USB-Ersteinrichtung
// bleibt der primaere Weg, der AP ist ein zusaetzliches Sicherheitsnetz fuer
// den Fall, dass ein einmal konfiguriertes WLAN spaeter dauerhaft
// unerreichbar wird.
static const char* FALLBACK_AP_SSID = "installer";
static const char* FALLBACK_AP_PSK = "installer";

typedef enum {
  NET_STATE_WLAN_CHECK,
  NET_STATE_FALLBACK_MODE,
  NET_STATE_RUN_NORMAL,
} net_state_t;

static esp_timer_handle_t s_tick_timer = NULL;
static esp_netif_t* s_ap_netif = NULL;
static net_state_t s_state = NET_STATE_WLAN_CHECK;
static bool s_ap_active = false;
static int64_t s_network_check_started_us = 0;
static int64_t s_last_reconnect_attempt_us = 0;

static volatile bool s_connected = false;

// Zwischengespeicherte, nach Empfangsstaerke sortierte Scan-Ergebnisse des
// periodischen Hintergrund-Scans (siehe network_manager_tick/event_handler).
static network_wifi_scan_result_t s_scan_cache[MAX_SCAN_CACHE];
static int s_scan_cache_count = 0;
static int64_t s_last_scan_us = 0;
static bool s_scan_in_progress = false;

static bool s_static_ip_active = false;
static char s_static_ip[16] = "";
static char s_static_netmask[16] = "";
static char s_static_gateway[16] = "";
static char s_ssid[33] = "";
static char s_password[64] = "";

// Persistiert die aktuellen STA-Zugangsdaten (inital-setup.txt) - analog
// dem save/load-Muster aus wireguard_manager.c.
static void save_wlan_to_storage(void) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "ssid", s_ssid);
  cJSON_AddStringToObject(root, "password", s_password);
  char* text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) return;

  FILE* f = fopen(WLAN_CONFIG_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  } else {
    ESP_LOGE(TAG, "Konnte %s nicht schreiben", WLAN_CONFIG_FILE);
  }
  cJSON_free(text);
}

static bool load_wlan_from_storage(void) {
  FILE* f = fopen(WLAN_CONFIG_FILE, "r");
  if (!f) return false;

  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* root = cJSON_Parse(buf);
  if (!root) return false;

  cJSON* item;
  bool ok = false;
  if ((item = cJSON_GetObjectItem(root, "ssid")) && cJSON_IsString(item)) {
    strncpy(s_ssid, item->valuestring, sizeof(s_ssid) - 1);
    ok = true;
  }
  if ((item = cJSON_GetObjectItem(root, "password")) && cJSON_IsString(item)) {
    strncpy(s_password, item->valuestring, sizeof(s_password) - 1);
  }
  cJSON_Delete(root);
  return ok;
}

// Absteigend nach Empfangsstaerke: der groessere (weniger negative) RSSI-Wert
// ist der bessere Empfang und kommt nach oben.
static int rssi_cmp_desc(const void* a, const void* b) {
  const network_wifi_scan_result_t* x = (const network_wifi_scan_result_t*)a;
  const network_wifi_scan_result_t* y = (const network_wifi_scan_result_t*)b;
  return (int)y->rssi - (int)x->rssi;
}

// Liest die Ergebnisse eines gerade abgeschlossenen WLAN-Scans, sortiert sie
// nach Empfangsstaerke absteigend und entfernt SSID-Duplikate (die staerkste
// Instanz bleibt oben stehen) sowie versteckte (leere) SSIDs. Schreibt bis zu
// max Eintraege nach out, liefert die tatsaechliche Anzahl.
static int collect_scan_results(network_wifi_scan_result_t* out, int max) {
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) return 0;

  wifi_ap_record_t* records = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!records) return 0;
  if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
    free(records);
    return 0;
  }

  network_wifi_scan_result_t* all = malloc(sizeof(network_wifi_scan_result_t) * ap_count);
  if (!all) {
    free(records);
    return 0;
  }
  for (int i = 0; i < ap_count; i++) {
    strncpy(all[i].ssid, (const char*)records[i].ssid, sizeof(all[i].ssid) - 1);
    all[i].ssid[sizeof(all[i].ssid) - 1] = '\0';
    all[i].open = (records[i].authmode == WIFI_AUTH_OPEN);
    all[i].rssi = records[i].rssi;
  }
  free(records);

  qsort(all, ap_count, sizeof(network_wifi_scan_result_t), rssi_cmp_desc);

  int n = 0;
  for (int i = 0; i < ap_count && n < max; i++) {
    if (all[i].ssid[0] == '\0') continue;  // versteckte SSID
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (strcmp(out[j].ssid, all[i].ssid) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) out[n++] = all[i];
  }
  free(all);
  return n;
}

// Schaltet auf einen eigenen Access Point um, wenn nach WLAN_CHECK_TIMEOUT_US
// (bewaehrter Wert: 5 Minuten, sensormeter-wlan) keine STA-Verbindung
// zustande kam - analog startFallbackAp() in sensormeter-wlan/
// NetworkManager.cpp: kein Gateway/Routing ins Internet, eigener DHCP-Server
// (IP/Subnetz bereits in network_manager_init() vorkonfiguriert). Ueber
// diesen AP ist web_server_manager weiterhin unter 192.168.4.1 erreichbar -
// dort koennen neue WLAN-Zugangsdaten eingetragen werden
// (network_manager_join() schaltet danach zurueck auf STA).
static void start_fallback_ap(void) {
  esp_wifi_disconnect();  // Fehler ignoriert, falls gerade nicht verbunden
  // APSTA statt reinem AP-Modus: das STA-Interface bleibt dadurch nutzbar,
  // waehrend der AP laeuft - reiner WIFI_MODE_AP hat esp_wifi_scan_start()
  // (network_manager_scan_wifi(), fuer den "Scan starten"-Knopf im
  // Webinterface) mit ESP_ERR_WIFI_MODE scheitern lassen, da ein WLAN-Scan
  // ein aktives STA-Interface braucht - genau ueber diesen AP verbunden
  // wollte man ja typischerweise ein echtes Netz suchen und eintragen.
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Fallback-Access-Point: esp_wifi_set_mode(APSTA) fehlgeschlagen: %s", esp_err_to_name(err));
    s_ap_active = false;
    return;
  }
  s_ap_active = true;
  ESP_LOGW(TAG, "Kein WLAN erreichbar - Fallback-Access-Point \"%s\" gestartet (192.168.4.1)", FALLBACK_AP_SSID);
}

// Periodische, entkoppelte Zustandsmaschine statt Reaktion auf jedes einzelne
// WIFI_EVENT_STA_DISCONNECTED (siehe event_handler()) - bewaehrtes Muster aus
// sensormeter-wlan/NetworkManager.cpp::loop(). Grund fuer die Entkopplung:
// beim ersten echten Hardware-Bring-up (2026-07-23) war noch kein WLAN
// hochgeladen, die Firmware versuchte also dauerhaft die Kconfig-
// Platzhalter-SSID "CHANGE_ME_SSID" zu joinen. Das schlaegt sofort fehl ->
// DISCONNECTED-Event -> sofortiger erneuter esp_wifi_connect() -> naechstes
// DISCONNECTED ... Dieser ungebremste Reconnect-Sturm hat den Task-Watchdog
// auf IDLE0 ausgehungert. Siehe docs/entscheidungen.md.
static void network_manager_tick(void* arg) {
  (void)arg;
  int64_t now = esp_timer_get_time();

  switch (s_state) {
    case NET_STATE_WLAN_CHECK:
      if (s_connected) {
        ESP_LOGI(TAG, "WLAN verbunden - Normalbetrieb");
        s_state = NET_STATE_RUN_NORMAL;
      } else if (now - s_network_check_started_us >
                 (s_ssid[0] == '\0' ? WLAN_CHECK_TIMEOUT_NO_CONFIG_US : WLAN_CHECK_TIMEOUT_US)) {
        start_fallback_ap();
        s_state = NET_STATE_FALLBACK_MODE;
      } else if (s_ssid[0] != '\0' && now - s_last_reconnect_attempt_us > RECONNECT_RETRY_INTERVAL_US) {
        ESP_LOGW(TAG, "WLAN weg - aktiver Reconnect-Versuch");
        esp_wifi_connect();
        s_last_reconnect_attempt_us = now;
      }
      break;

    case NET_STATE_FALLBACK_MODE:
      // Regulaerer Ausstieg passiert in network_manager_join() (neue
      // Zugangsdaten eingetragen -> zurueck auf STA). Hier nur Sicherheitsnetz,
      // falls s_connected aus anderem Grund schon wieder true ist, und Retry,
      // falls das Starten des APs selbst fehlgeschlagen war.
      if (s_connected) {
        s_ap_active = false;
        s_state = NET_STATE_RUN_NORMAL;
      } else if (!s_ap_active) {
        start_fallback_ap();
      }
      break;

    case NET_STATE_RUN_NORMAL:
      if (!s_connected) {
        ESP_LOGW(TAG, "WLAN-Verbindung verloren");
        s_state = NET_STATE_WLAN_CHECK;
        s_network_check_started_us = now;
        s_last_reconnect_attempt_us = now;
      }
      break;
  }

  // Periodischer, nicht-blockierender WLAN-Scan, damit das Webinterface die
  // verfuegbaren Netzwerke aktuell anzeigen kann (Ergebnis wird bei
  // WIFI_EVENT_SCAN_DONE sortiert zwischengespeichert). Bewusst NUR wenn nicht
  // verbunden UND entweder der Installer-AP laeuft oder gar kein WLAN
  // konfiguriert ist - also genau in der Ersteinrichtungsphase. Waehrend einer
  // aktiven STA-Verbindung wird nicht gescannt, um sie (und den daran
  // haengenden VPN-Tunnel) nicht durch das Kanal-Hopping des Scans zu stoeren.
  if (!s_connected && (s_ap_active || s_ssid[0] == '\0') && !s_scan_in_progress &&
      now - s_last_scan_us > (int64_t)WLAN_SCAN_INTERVAL_US) {
    wifi_scan_config_t cfg = {0};
    if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {  // false = asynchron
      s_scan_in_progress = true;
      s_last_scan_us = now;
    }
  }
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  (void)arg;
  (void)event_data;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    if (s_ssid[0] != '\0') esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    // Kein sofortiger esp_wifi_connect() mehr hier - siehe network_manager_tick().
    s_connected = false;
    ESP_LOGW(TAG, "WLAN getrennt");
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
    // Nur den vom periodischen Hintergrund-Scan angestossenen Lauf auswerten
    // (der blockierende network_manager_scan_wifi() holt seine Ergebnisse
    // selbst ab und setzt s_scan_in_progress nicht).
    if (s_scan_in_progress) {
      s_scan_cache_count = collect_scan_results(s_scan_cache, MAX_SCAN_CACHE);
      s_scan_in_progress = false;
      ESP_LOGI(TAG, "WLAN-Scan: %d Netzwerke gefunden", s_scan_cache_count);
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    ESP_LOGI(TAG, "Client mit Fallback-Access-Point verbunden");
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    ESP_LOGI(TAG, "Client vom Fallback-Access-Point getrennt");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_connected = true;
    ESP_LOGI(TAG, "WLAN verbunden, IP erhalten");
    time_manager_notify_link_up();
  }
}

void network_manager_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  s_ap_netif = esp_netif_create_default_wifi_ap();

  wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

  // esp_wifi_set_config() prueft intern, ob das Zielinterface im aktuell
  // gesetzten Wifi-Modus ueberhaupt enthalten ist (sonst ESP_ERR_WIFI_MODE) -
  // deshalb hier vorab auf APSTA schalten, damit sowohl das AP- als auch das
  // STA-Config weiter unten gueltig gesetzt werden koennen. Vor
  // esp_wifi_start() wird der tatsaechliche Startmodus dann auf reines STA
  // zurueckgeschaltet (siehe unten) - das bereits gesetzte AP-Config bleibt
  // dabei erhalten, start_fallback_ap() muss es spaeter nicht erneut setzen.
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  // Fallback-AP-Netzwerk einmalig vorkonfigurieren (192.168.4.1/24, eigener
  // DHCP-Server, kein Gateway/Routing - analog sensormeter-wlan) - gilt
  // dauerhaft, unabhaengig davon, ob/wann start_fallback_ap() den Modus
  // tatsaechlich auf WIFI_MODE_AP umschaltet.
  esp_netif_dhcps_stop(s_ap_netif);
  esp_netif_ip_info_t ap_ip_info = {0};
  esp_netif_str_to_ip4("192.168.4.1", &ap_ip_info.ip);
  esp_netif_str_to_ip4("192.168.4.1", &ap_ip_info.gw);
  esp_netif_str_to_ip4("255.255.255.0", &ap_ip_info.netmask);
  ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ap_ip_info));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

  wifi_config_t ap_config = {0};
  strncpy((char*)ap_config.ap.ssid, FALLBACK_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
  ap_config.ap.ssid_len = strlen(FALLBACK_AP_SSID);
  strncpy((char*)ap_config.ap.password, FALLBACK_AP_PSK, sizeof(ap_config.ap.password) - 1);
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.max_connection = 4;
  ap_config.ap.channel = 1;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  if (load_wlan_from_storage()) {
    ESP_LOGI(TAG, "WLAN-Zugangsdaten von %s geladen", WLAN_CONFIG_FILE);
  } else {
    // Kein Upload vorhanden - Kconfig-Platzhalterkonfiguration aus dem
    // P0-Mindestumfang (siehe docs/entscheidungen.md).
    strncpy(s_ssid, CONFIG_ESP_BMC_WIFI_SSID, sizeof(s_ssid) - 1);
    strncpy(s_password, CONFIG_ESP_BMC_WIFI_PASSWORD, sizeof(s_password) - 1);

    // ACHTUNG (2026-07-23, erster echter Hardware-Bring-up): der
    // unveraenderte Kconfig-Platzhalter "CHANGE_ME_SSID" ist kein echtes
    // Netzwerk und kann es per Definition auf keinem Testaufbau je sein -
    // ein Verbindungsversuch dagegen (STA_START/Reconnect) loest bei
    // fehlendem BSSID einen vollen Kanal-Scan aus, der auf echter Hardware
    // reproduzierbar so viel CPU0-Zeit gebraucht hat, dass der Task-
    // Watchdog auf IDLE0 ausgeloest hat (siehe docs/entscheidungen.md,
    // Abschnitt "WLAN-Reconnect-Sturm..."). Deshalb wird der Platzhalter
    // hier wie "kein WLAN konfiguriert" behandelt (leere SSID) statt einen
    // von vornherein zum Scheitern verurteilten Verbindungsversuch
    // anzustossen - die Zustandsmaschine geht dann sofort in den
    // WLAN_CHECK-Wartezustand und nach WLAN_CHECK_TIMEOUT_US in den
    // Fallback-Access-Point.
    if (strcmp(s_ssid, "CHANGE_ME_SSID") == 0) {
      ESP_LOGW(TAG, "WLAN-SSID ist noch der Kconfig-Platzhalter - kein Verbindungsversuch, warte auf Fallback-Access-Point");
      s_ssid[0] = '\0';
      s_password[0] = '\0';
    }
  }

  wifi_config_t wifi_config = {0};
  strncpy((char*)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char*)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = s_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  int64_t now = esp_timer_get_time();
  s_state = NET_STATE_WLAN_CHECK;
  s_network_check_started_us = now;
  s_last_reconnect_attempt_us = now;
  // So gesetzt, dass der erste periodische Scan schon beim ersten passenden
  // Tick laeuft (statt erst nach WLAN_SCAN_INTERVAL_US), damit die
  // Netzwerkliste in der Ersteinrichtung ohne Verzoegerung erscheint.
  s_last_scan_us = now - (int64_t)WLAN_SCAN_INTERVAL_US;

  const esp_timer_create_args_t tick_timer_args = {
      .callback = &network_manager_tick,
      .name = "network_tick",
  };
  ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &s_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, NETWORK_TICK_INTERVAL_US));

  ESP_LOGI(TAG, "NetworkManager gestartet (SSID: %s)", s_ssid);
}

void network_manager_join(const char* ssid, const char* password) {
  strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
  s_ssid[sizeof(s_ssid) - 1] = '\0';
  strncpy(s_password, password, sizeof(s_password) - 1);
  s_password[sizeof(s_password) - 1] = '\0';
  save_wlan_to_storage();

  wifi_config_t wifi_config = {0};
  strncpy((char*)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char*)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = s_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  // Falls gerade der Fallback-Access-Point aktiv war (neue Zugangsdaten
  // typischerweise ueber genau diesen AP eingetragen, siehe start_fallback_ap()):
  // zurueck auf reinen STA-Modus, bevor der neue Verbindungsversuch startet.
  esp_wifi_set_mode(WIFI_MODE_STA);
  s_ap_active = false;
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  s_connected = false;
  esp_wifi_disconnect();  // Fehler ignoriert, falls noch nie verbunden
  esp_wifi_connect();

  int64_t now = esp_timer_get_time();
  s_state = NET_STATE_WLAN_CHECK;
  s_network_check_started_us = now;
  s_last_reconnect_attempt_us = now;

  ESP_LOGI(TAG, "Neue WLAN-Zugangsdaten uebernommen (SSID: %s), Reconnect angestossen", s_ssid);
}

void network_manager_get_ssid(char* out, size_t out_len) {
  strncpy(out, s_ssid, out_len - 1);
  out[out_len - 1] = '\0';
}

void network_manager_reset(void) {
  remove(WLAN_CONFIG_FILE);
  ESP_LOGI(TAG, "Gespeicherte WLAN-Zugangsdaten geloescht (wirkt nach Neustart)");
}

bool network_manager_is_connected(void) { return s_connected; }

bool network_manager_is_fallback_ap_active(void) { return s_ap_active; }

bool network_manager_get_ip_string(char* out_buf, size_t buf_len) {
  out_buf[0] = '\0';
  if (!s_connected) return false;

  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return false;

  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;

  snprintf(out_buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
  return true;
}

// ---------------------------------------------------------------------
// Ping-Check (vor Uebernahme einer statischen IP, analog Sensormeter)
// ---------------------------------------------------------------------

typedef struct {
  SemaphoreHandle_t done_sem;
  int success_count;
} ping_ctx_t;

static void on_ping_success(esp_ping_handle_t hdl, void* args) {
  (void)hdl;
  ((ping_ctx_t*)args)->success_count++;
}

static void on_ping_end(esp_ping_handle_t hdl, void* args) {
  (void)hdl;
  xSemaphoreGive(((ping_ctx_t*)args)->done_sem);
}

// Blockierend (max. ~5s) - true, wenn mindestens eine ICMP-Antwort kam.
static bool ping_check(const char* ip_str) {
  esp_ip4_addr_t target_ip4;
  if (esp_netif_str_to_ip4(ip_str, &target_ip4) != ESP_OK) return false;

  ping_ctx_t ctx = {.done_sem = xSemaphoreCreateBinary(), .success_count = 0};
  if (!ctx.done_sem) return false;

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.count = 3;
  config.timeout_ms = 1000;
  config.target_addr.type = IPADDR_TYPE_V4;
  config.target_addr.u_addr.ip4.addr = target_ip4.addr;

  esp_ping_callbacks_t cbs = {
      .cb_args = &ctx,
      .on_ping_success = on_ping_success,
      .on_ping_timeout = NULL,
      .on_ping_end = on_ping_end,
  };

  esp_ping_handle_t ping;
  if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK) {
    vSemaphoreDelete(ctx.done_sem);
    return false;
  }
  esp_ping_start(ping);
  xSemaphoreTake(ctx.done_sem, pdMS_TO_TICKS(5000));
  esp_ping_delete_session(ping);
  vSemaphoreDelete(ctx.done_sem);
  return ctx.success_count > 0;
}

// ---------------------------------------------------------------------
// Statische IP / DHCP
// ---------------------------------------------------------------------

bool network_manager_is_static_ip(void) { return s_static_ip_active; }

bool network_manager_apply_static_ip(const char* ip, const char* netmask, const char* gateway) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return false;

  esp_netif_ip_info_t new_info = {0};
  if (esp_netif_str_to_ip4(ip, &new_info.ip) != ESP_OK) return false;
  if (esp_netif_str_to_ip4(netmask, &new_info.netmask) != ESP_OK) return false;
  if (esp_netif_str_to_ip4(gateway, &new_info.gw) != ESP_OK) return false;

  esp_netif_ip_info_t previous_info;
  bool had_previous = (esp_netif_get_ip_info(netif, &previous_info) == ESP_OK);
  bool previous_was_static = s_static_ip_active;

  esp_netif_dhcpc_stop(netif);
  esp_netif_set_ip_info(netif, &new_info);

  if (!ping_check(gateway)) {
    ESP_LOGW(TAG, "Ping-Check fuer statische IP %s fehlgeschlagen - stelle vorherige Konfiguration wieder her", ip);
    if (previous_was_static && had_previous) {
      esp_netif_set_ip_info(netif, &previous_info);
    } else {
      esp_netif_dhcpc_start(netif);
    }
    return false;
  }

  s_static_ip_active = true;
  strncpy(s_static_ip, ip, sizeof(s_static_ip) - 1);
  strncpy(s_static_netmask, netmask, sizeof(s_static_netmask) - 1);
  strncpy(s_static_gateway, gateway, sizeof(s_static_gateway) - 1);
  ESP_LOGI(TAG, "Statische IP uebernommen: %s", ip);
  return true;
}

void network_manager_use_dhcp(void) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) esp_netif_dhcpc_start(netif);
  s_static_ip_active = false;
  ESP_LOGI(TAG, "Auf DHCP umgestellt");
}

void network_manager_get_static_config(char* out_ip, char* out_netmask, char* out_gateway) {
  strcpy(out_ip, s_static_ip);
  strcpy(out_netmask, s_static_netmask);
  strcpy(out_gateway, s_static_gateway);
}

// ---------------------------------------------------------------------
// WLAN-Scan
// ---------------------------------------------------------------------

int network_manager_scan_wifi(network_wifi_scan_result_t* out, int max_results) {
  // Laeuft gerade ein asynchroner Hintergrund-Scan, wuerde ein zweiter
  // esp_wifi_scan_start() nur scheitern - dann direkt den Cache liefern.
  if (s_scan_in_progress) return network_manager_get_cached_scan(out, max_results);

  wifi_scan_config_t scan_config = {0};
  if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return 0;  // blockierend
  int n = collect_scan_results(out, max_results);

  // Frisches Ergebnis zugleich in den Cache uebernehmen, damit die
  // Webinterface-Anzeige (die den Cache liest) sofort aktuell ist.
  s_scan_cache_count = n < MAX_SCAN_CACHE ? n : MAX_SCAN_CACHE;
  for (int i = 0; i < s_scan_cache_count; i++) s_scan_cache[i] = out[i];
  s_last_scan_us = esp_timer_get_time();
  return n;
}

int network_manager_get_cached_scan(network_wifi_scan_result_t* out, int max_results) {
  int n = s_scan_cache_count < max_results ? s_scan_cache_count : max_results;
  for (int i = 0; i < n; i++) out[i] = s_scan_cache[i];
  return n;
}
