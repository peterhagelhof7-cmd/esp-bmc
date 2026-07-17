#include "usb_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_log.h"
#include "class/hid/hid_device.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gpio_manager.h"
#include "network_manager.h"
#include "sensor_history.h"
#include "sensor_manager.h"
#include "storage_manager.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "user_manager.h"
#include "wireguard_manager.h"

static const char* TAG = "usb_manager";

// esp_tinyusb generiert den Konfigurations-Deskriptor NUR automatisch, wenn
// KEINE der Klassen HID/MIDI/ECM_RNDIS/DFU/BTH aktiv ist (siehe
// managed_components/espressif__esp_tinyusb/descriptors_control.c,
// tinyusb_set_descriptors() - Kommentar "Default configuration descriptor
// must be provided for the following classes"). Mit aktivem HID (unser
// Tastatur-Fallback) MUSS der Composite-Deskriptor deshalb von Hand
// zusammengesetzt werden - der urspruengliche Versuch, alles auf Kconfig
// zu verlassen, schlug erst beim ersten echten Boot (Wokwi) mit
// "Configuration descriptor must be provided for this device" fehl, siehe
// docs/entscheidungen.md.
enum {
  USB_ITF_CDC = 0,  // belegt Interface 0 UND 1 (Control + Data, siehe TUD_CDC_DESCRIPTOR)
  USB_ITF_HID = 2,
  USB_ITF_COUNT
};

#define USB_EP_CDC_NOTIF 0x81
#define USB_EP_CDC_OUT 0x02
#define USB_EP_CDC_IN 0x82
#define USB_EP_HID_IN 0x83

#define USB_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const s_hid_report_desc[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

static uint8_t const s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_COUNT, 0, USB_CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(USB_ITF_CDC, 0, USB_EP_CDC_NOTIF, 8, USB_EP_CDC_OUT, USB_EP_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(USB_ITF_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report_desc), USB_EP_HID_IN, 16, 10),
};

// Bidirektionale Weiterleitung CDC <-> interne Konsolen-Queue (Pflichtenheft
// Abschnitt 3.7) - P5 (WebServerManager) liest/schreibt darueber. Groesse
// grosszuegig fuer ein paar Zeilen Konsolentext gewaehlt.
#define CDC_RX_QUEUE_LEN 512

static QueueHandle_t s_cdc_rx_queue;
static volatile bool s_cdc_host_ready;  // DTR-Zustand

// =============================================================================
// USB-Kommandoprotokoll (Host -> ESP-BMC) - docs/entscheidungen.md
// "USB-Kommandoprotokoll", docs/pflichtenheft.txt 3.6/3.7 ("CDC-Kommando
// zum Auslesen des StorageManager-Inhalts, Format noch offen").
//
// Kommandozeilen werden am Praefix "##ESPR " erkannt, direkt im selben
// Bytestrom, der auch die Konsole des gesteuerten PCs durchreicht - echter
// Konsolentext beginnt so gut wie nie zufaellig mit dieser Sequenz. Bewusst
// KEINE Sonderbehandlung, die diese Zeilen aus der Konsolen-Queue
// heraushaelt (ausser beim laengeren "wg upload"-Rohdatenblock, siehe
// unten): das wuerde Zeilenpufferung vor jedem Zeichen-Forward erzwingen
// und die Interaktivitaet der echten Konsole spuerbar verschlechtern. Ein
// paar Steuerzeichen, die in der Live-Konsole auftauchen, sind der
// akzeptierte Kompromiss (galt schon fuer das alte "storage"-Kommando).
//
// Antwortformat: eine oder mehrere Zeilen, erste Zeile immer
// "##ESPR OK [zusatz]" oder "##ESPR ERR <grund>", danach ggf. Nutzdaten,
// abgeschlossen durch eine Zeile "##ESPR END" - so kann ein Host-Werkzeug
// (z.B. das inital-setup-Skript) immer bis zur END-Zeile lesen.
// =============================================================================

#define CMD_PREFIX "##ESPR "
#define CMD_PREFIX_LEN 7

#define CMD_BUF_CAP 512
static char s_cmd_buf[CMD_BUF_CAP];
static size_t s_cmd_len;

static bool s_logged_in;
static user_role_t s_role;
static char s_username[32];

#define WG_UPLOAD_MAX 2048
static bool s_wg_upload_active;
static size_t s_wg_upload_expected;
static size_t s_wg_upload_received;
static char s_wg_upload_buf[WG_UPLOAD_MAX + 1];

static const char* role_name(user_role_t role) {
  switch (role) {
    case USER_ROLE_ADMIN: return "Admin";
    case USER_ROLE_VERWALTER: return "Verwalter";
    case USER_ROLE_SSH_USER: return "SSH User";
    default: return "Leser";
  }
}

static void reply_line(const char* line) {
  char buf[600];
  int n = snprintf(buf, sizeof(buf), CMD_PREFIX "%s\r\n", line);
  usb_manager_cdc_write((const uint8_t*)buf, n > 0 && (size_t)n < sizeof(buf) ? (size_t)n : strlen(buf));
}

static void reply_ok(const char* rest) {
  if (rest && rest[0]) {
    char l[80];
    snprintf(l, sizeof(l), "OK %s", rest);
    reply_line(l);
  } else {
    reply_line("OK");
  }
}

static void reply_err(const char* reason) {
  char l[96];
  snprintf(l, sizeof(l), "ERR %s", reason);
  reply_line(l);
}

static void reply_end(void) { reply_line("END"); }

static void reply_payload(const char* text) { usb_manager_cdc_write((const uint8_t*)text, strlen(text)); }

// Prueft, ob eine USB-Session eingeloggt ist und mindestens min_role hat -
// analog require_role() in web_server_manager.c, aber auf den einen
// globalen USB-Sessionszustand statt eines Cookies bezogen (es gibt nur
// eine physische USB-Verbindung gleichzeitig).
static bool require_role(user_role_t min_role) {
  if (!s_logged_in) {
    reply_err("nicht angemeldet - erst \"login <user> <pass>\"");
    reply_end();
    return false;
  }
  if (s_role < min_role) {
    reply_err("Rolle reicht nicht aus");
    reply_end();
    return false;
  }
  return true;
}

// --- Kommando-Handler ---

static void cmd_login(char* args) {
  char* user = strtok(args, " ");
  char* pass = user ? strtok(NULL, "") : NULL;  // Rest der Zeile als Passwort (kann Leerzeichen enthalten)
  if (!user || !pass) {
    reply_err("Syntax: login <benutzer> <passwort>");
    reply_end();
    return;
  }

  user_role_t role;
  if (user_manager_authenticate(user, pass, &role)) {
    s_logged_in = true;
    s_role = role;
    strncpy(s_username, user, sizeof(s_username) - 1);
    s_username[sizeof(s_username) - 1] = '\0';
    ESP_LOGI(TAG, "Login erfolgreich: %s (%s)", user, role_name(role));
    char event[64];
    snprintf(event, sizeof(event), "Login (USB): %s (%s)", user, role_name(role));
    audit_log_add(event);
    reply_ok(role_name(role));
  } else {
    ESP_LOGW(TAG, "USB-Login fehlgeschlagen fuer Benutzername \"%s\"", user);
    reply_err("ungueltige Zugangsdaten");
  }
  reply_end();
}

static void cmd_logout(void) {
  s_logged_in = false;
  s_username[0] = '\0';
  reply_ok(NULL);
  reply_end();
}

static void cmd_status(void) {
  if (!require_role(USER_ROLE_LESER)) return;

  char ip[16] = "-";
  network_manager_get_ip_string(ip, sizeof(ip));
  char ssid[33] = "";
  network_manager_get_ssid(ssid, sizeof(ssid));
  char wg_ip[16] = "-";
  wireguard_manager_get_local_address(wg_ip, sizeof(wg_ip));
  char wg_endpoint[80] = "-";
  wireguard_manager_get_endpoint(wg_endpoint, sizeof(wg_endpoint));

  float ntc_temp = 0, dht_temp = 0, dht_hum = 0;
  bool ntc_ok = sensor_manager_get_ntc_temp_c(&ntc_temp);
  bool dht_ok = sensor_manager_get_dht_temp_c(&dht_temp) && sensor_manager_get_dht_humidity_pct(&dht_hum);
  int64_t uptime_s = esp_timer_get_time() / 1000000;

  reply_ok(NULL);
  char line[160];
  snprintf(line, sizeof(line), "wlan_ip=%s wlan_ssid=%s wlan_static=%d\r\n", ip, ssid,
           network_manager_is_static_ip());
  reply_payload(line);

  snprintf(line, sizeof(line), "vpn_configured=%d vpn_up=%d vpn_local_ip=%s vpn_endpoint=%s\r\n",
           wireguard_manager_has_uploaded_config(), wireguard_manager_is_up(), wg_ip, wg_endpoint);
  reply_payload(line);

  if (ntc_ok) {
    snprintf(line, sizeof(line), "ntc_temp_c=%.1f\r\n", ntc_temp);
  } else {
    snprintf(line, sizeof(line), "ntc_temp_c=-\r\n");
  }
  reply_payload(line);

  if (dht_ok) {
    snprintf(line, sizeof(line), "dht_temp_c=%.1f dht_humidity_pct=%.1f\r\n", dht_temp, dht_hum);
  } else {
    snprintf(line, sizeof(line), "dht_temp_c=- dht_humidity_pct=-\r\n");
  }
  reply_payload(line);

  snprintf(line, sizeof(line), "uptime_s=%lld power_led=%d hdd_led=%d hdd_led_active_10s=%d\r\n",
           (long long)uptime_s, gpio_manager_read_power_led(), gpio_manager_read_hdd_led(),
           gpio_manager_hdd_led_active_recently());
  reply_payload(line);
  reply_end();
}

static void cmd_log_audit(void) {
  if (!require_role(USER_ROLE_VERWALTER)) return;
  reply_ok(NULL);
  static char buf[4096];
  audit_log_read(buf, sizeof(buf));
  reply_payload(buf);
  if (buf[0] && buf[strlen(buf) - 1] != '\n') reply_payload("\r\n");
  reply_end();
}

static void cmd_log_sensors(void) {
  if (!require_role(USER_ROLE_LESER)) return;
  reply_ok(NULL);
  static char buf[2048];
  sensor_history_get_csv(buf, sizeof(buf));
  reply_payload(buf);
  if (buf[0] && buf[strlen(buf) - 1] != '\n') reply_payload("\r\n");
  reply_end();
}

static void cmd_config_download(void) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  char ip[16] = "-";
  network_manager_get_ip_string(ip, sizeof(ip));
  char wg_ip[16] = "-";
  wireguard_manager_get_local_address(wg_ip, sizeof(wg_ip));

  reply_ok(NULL);
  char body[1024];
  size_t off = snprintf(body, sizeof(body),
                         "{\"wlan_ip\":\"%s\",\"wlan_static\":%s,\"wireguard_configured\":%s,"
                         "\"wireguard_local_ip\":\"%s\",\"wireguard_connected\":%s,\"users\":[",
                         ip, network_manager_is_static_ip() ? "true" : "false",
                         wireguard_manager_has_uploaded_config() ? "true" : "false", wg_ip,
                         wireguard_manager_is_up() ? "true" : "false");
  for (size_t i = 0; i < user_manager_count() && off < sizeof(body) - 64; i++) {
    char uname[32];
    user_role_t urole;
    if (user_manager_get_at(i, uname, &urole)) {
      off += snprintf(body + off, sizeof(body) - off, "%s{\"username\":\"%s\",\"role\":\"%s\"}", i == 0 ? "" : ",",
                       uname, role_name(urole));
    }
  }
  snprintf(body + off, sizeof(body) - off, "]}\r\n");
  reply_payload(body);
  reply_end();
}

static void cmd_reboot(void) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  ESP_LOGW(TAG, "Neustart ausgeloest von Benutzer \"%s\" (USB)", s_username);
  char event[64];
  snprintf(event, sizeof(event), "Neustart ausgeloest von %s (USB)", s_username);
  audit_log_add(event);
  reply_ok(NULL);
  reply_end();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
}

// Reset (webconfig.txt "Seite Einstellungen", gleiche Semantik wie
// settings_reset_post_handler in web_server_manager.c - Audit-Log bleibt
// immer erhalten, alles andere je nach scope).
static void cmd_reset(char* args) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  char* scope = strtok(args, " ");
  bool include_values = scope && strcmp(scope, "settings_values") == 0;
  if (!scope || (strcmp(scope, "settings") != 0 && !include_values)) {
    reply_err("Syntax: reset settings | reset settings_values");
    reply_end();
    return;
  }

  config_manager_reset_to_defaults();
  network_manager_reset();
  wireguard_manager_delete_config();
  user_manager_reset_to_default();
  if (include_values) sensor_history_reset();

  char event[112];
  snprintf(event, sizeof(event), "Reset (%s) ausgeloest von %s (USB)",
           include_values ? "Einstellungen+Werte" : "Einstellungen", s_username);
  audit_log_add(event);
  ESP_LOGW(TAG, "%s", event);
  reply_ok(NULL);
  reply_end();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
}

static void cmd_wg_delete(void) {
  if (!require_role(USER_ROLE_VERWALTER)) return;
  wireguard_manager_delete_config();
  audit_log_add("WireGuard-Konfiguration geloescht (USB)");
  reply_ok(NULL);
  reply_end();
}

static void cmd_wg_upload_start(char* args) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  char* len_str = strtok(args, " ");
  long len = len_str ? atol(len_str) : -1;
  if (len <= 0 || (size_t)len > WG_UPLOAD_MAX) {
    reply_err("Syntax: wg upload <laenge in byte, max 2048>");
    reply_end();
    return;
  }

  s_wg_upload_active = true;
  s_wg_upload_expected = (size_t)len;
  s_wg_upload_received = 0;
  // Keine Bestaetigung hier - die kommt erst nach Empfang aller Rohbytes
  // (siehe cdc_rx_callback), damit der Host genau weiss, wann er die
  // naechste Zeile lesen darf.
}

// --- WLAN-Scan/-Join (inital-setup.txt) ---

#define WLAN_SCAN_MAX 10
static network_wifi_scan_result_t s_last_scan[WLAN_SCAN_MAX];
static int s_last_scan_count;

static void cmd_wlan_scan(void) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  s_last_scan_count = network_manager_scan_wifi(s_last_scan, WLAN_SCAN_MAX);
  reply_ok(NULL);
  char line[80];
  for (int i = 0; i < s_last_scan_count; i++) {
    snprintf(line, sizeof(line), "%d;%s;%s;%d\r\n", i, s_last_scan[i].ssid, s_last_scan[i].open ? "OPEN" : "WPA",
             s_last_scan[i].rssi);
    reply_payload(line);
  }
  reply_end();
}

static void cmd_wlan_join(char* args) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  char* idx_str = strtok(args, " ");
  char* psk = idx_str ? strtok(NULL, "") : NULL;  // Rest der Zeile als PSK, leer erlaubt (offenes Netz)
  int idx = idx_str ? atoi(idx_str) : -1;

  if (idx < 0 || idx >= s_last_scan_count) {
    reply_err("ungueltiger Index - erst \"wlan scan\" ausfuehren");
    reply_end();
    return;
  }

  network_manager_join(s_last_scan[idx].ssid, psk ? psk : "");
  char event[80];
  snprintf(event, sizeof(event), "WLAN-Zugangsdaten geaendert (USB): %s", s_last_scan[idx].ssid);
  audit_log_add(event);
  reply_ok(s_last_scan[idx].ssid);
  reply_end();
}

// --- Taster-Steuerung (webconfig.txt Rollenliste, gleiche Semantik wie
//     settings_taster_post_handler in web_server_manager.c: Admin ohne
//     Ruecksicherung, Verwalter mit erneuter Passwort-Bestaetigung) ---

static void cmd_taster(char* args) {
  if (!require_role(USER_ROLE_VERWALTER)) return;

  char* action = strtok(args, " ");
  char* confirm_password = action ? strtok(NULL, "") : NULL;

  if (s_role == USER_ROLE_VERWALTER) {
    user_role_t tmp;
    if (!confirm_password || !user_manager_authenticate(s_username, confirm_password, &tmp)) {
      reply_err("Passwort-Bestaetigung fehlt oder falsch");
      reply_end();
      return;
    }
  }

  bool ok;
  const char* label;
  if (action && strcmp(action, "power_push") == 0) {
    ok = gpio_manager_trigger_power(false);
    label = "Power (kurz)";
  } else if (action && strcmp(action, "power_hold") == 0) {
    ok = gpio_manager_trigger_power(true);
    label = "Power (lang)";
  } else if (action && strcmp(action, "reset") == 0) {
    ok = gpio_manager_trigger_reset();
    label = "Reset";
  } else {
    reply_err("Syntax: taster power_push|power_hold|reset [passwort]");
    reply_end();
    return;
  }

  char event[128];
  snprintf(event, sizeof(event), "Taster \"%s\" ausgeloest von %s (USB)%s", label, s_username,
           ok ? "" : " - durch Tastschutz blockiert");
  audit_log_add(event);
  ESP_LOGI(TAG, "%s", event);
  if (ok) {
    reply_ok(NULL);
  } else {
    reply_err("durch Tastschutz blockiert");
  }
  reply_end();
}

// --- Diagnose (bestehendes "storage"-Kommando, jetzt unter dem
//     einheitlichen Praefix statt als bare Wort) ---

static void cmd_storage(void) {
  reply_ok(NULL);
  char line[96];
  if (storage_manager_is_mounted()) {
    size_t used = 0, total = 0;
    storage_manager_get_usage(&used, &total);
    snprintf(line, sizeof(line), "storage: gemountet unter %s, %u/%u Byte belegt\r\n",
             storage_manager_base_path(), (unsigned)used, (unsigned)total);
  } else {
    snprintf(line, sizeof(line), "storage: nicht gemountet\r\n");
  }
  reply_payload(line);
  reply_end();
}

static void dispatch_command(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) return;
  char* rest = strtok(NULL, "");  // Rest der Zeile (kann NULL sein)
  static char rest_buf[CMD_BUF_CAP];
  rest_buf[0] = '\0';
  if (rest) {
    strncpy(rest_buf, rest, sizeof(rest_buf) - 1);
    rest_buf[sizeof(rest_buf) - 1] = '\0';
  }

  if (strcmp(cmd, "login") == 0) {
    cmd_login(rest_buf);
  } else if (strcmp(cmd, "logout") == 0) {
    cmd_logout();
  } else if (strcmp(cmd, "status") == 0) {
    cmd_status();
  } else if (strcmp(cmd, "storage") == 0) {
    cmd_storage();
  } else if (strcmp(cmd, "log") == 0) {
    char* sub = strtok(rest_buf, " ");
    if (sub && strcmp(sub, "audit") == 0) {
      cmd_log_audit();
    } else if (sub && strcmp(sub, "sensors") == 0) {
      cmd_log_sensors();
    } else {
      reply_err("Syntax: log audit | log sensors");
      reply_end();
    }
  } else if (strcmp(cmd, "config") == 0) {
    char* sub = strtok(rest_buf, " ");
    if (sub && strcmp(sub, "download") == 0) {
      cmd_config_download();
    } else {
      reply_err("Syntax: config download");
      reply_end();
    }
  } else if (strcmp(cmd, "reboot") == 0) {
    cmd_reboot();
  } else if (strcmp(cmd, "taster") == 0) {
    cmd_taster(rest_buf);
  } else if (strcmp(cmd, "reset") == 0) {
    cmd_reset(rest_buf);
  } else if (strcmp(cmd, "wg") == 0) {
    char* sub = strtok(rest_buf, " ");
    char* sub_rest = sub ? strtok(NULL, "") : NULL;
    if (sub && strcmp(sub, "upload") == 0) {
      cmd_wg_upload_start(sub_rest ? sub_rest : "");
    } else if (sub && strcmp(sub, "delete") == 0) {
      cmd_wg_delete();
    } else {
      reply_err("Syntax: wg upload <laenge> | wg delete");
      reply_end();
    }
  } else if (strcmp(cmd, "wlan") == 0) {
    char* sub = strtok(rest_buf, " ");
    char* sub_rest = sub ? strtok(NULL, "") : NULL;
    if (sub && strcmp(sub, "scan") == 0) {
      cmd_wlan_scan();
    } else if (sub && strcmp(sub, "join") == 0) {
      cmd_wlan_join(sub_rest ? sub_rest : "");
    } else {
      reply_err("Syntax: wlan scan | wlan join <index> [psk]");
      reply_end();
    }
  } else {
    reply_err("unbekanntes Kommando");
    reply_end();
  }
}

// Wird nach Empfang aller erwarteten Rohbytes einer "wg upload"-Uebertragung
// aufgerufen (siehe cdc_rx_callback).
static void finish_wg_upload(void) {
  s_wg_upload_buf[s_wg_upload_received] = '\0';
  s_wg_upload_active = false;

  esp_err_t err = wireguard_manager_apply_uploaded_config(s_wg_upload_buf);
  if (err == ESP_OK) {
    audit_log_add("WireGuard-Konfiguration hochgeladen (USB)");
    reply_ok(NULL);
  } else {
    reply_err("Konfiguration unvollstaendig oder ungueltig");
  }
  reply_end();
}

static void process_cmd_byte(char c) {
  if (c == '\n' || c == '\r') {
    if (s_cmd_len > 0) {
      s_cmd_buf[s_cmd_len] = '\0';
      if (strncmp(s_cmd_buf, CMD_PREFIX, CMD_PREFIX_LEN) == 0) {
        dispatch_command(s_cmd_buf + CMD_PREFIX_LEN);
      }
      s_cmd_len = 0;
    }
    return;
  }
  if (s_cmd_len < CMD_BUF_CAP - 1) {
    s_cmd_buf[s_cmd_len++] = c;
  }
}

static void cdc_rx_callback(int itf, cdcacm_event_t* event) {
  (void)event;
  uint8_t buf[64];
  size_t rx_len = 0;
  esp_err_t err = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_len);
  if (err != ESP_OK) return;

  for (size_t i = 0; i < rx_len; i++) {
    // Waehrend eines "wg upload"-Rohdatenblocks: Bytes NICHT in die
    // Konsolen-Queue spiegeln (das waere kein Konsolentext, sondern eine
    // mehrere hundert Byte grosse Binaer-/Text-Nutzlast) und nicht als
    // Kommandozeile parsen, sondern direkt in den Upload-Puffer schreiben.
    if (s_wg_upload_active) {
      if (s_wg_upload_received < s_wg_upload_expected) {
        s_wg_upload_buf[s_wg_upload_received++] = (char)buf[i];
      }
      if (s_wg_upload_received >= s_wg_upload_expected) {
        finish_wg_upload();
      }
      continue;
    }

    // Nicht-blockierend - falls die Queue voll ist (kein Verbraucher vor
    // P5), wird das aelteste Byte verworfen statt den USB-Task zu blockieren.
    if (xQueueSend(s_cdc_rx_queue, &buf[i], 0) != pdTRUE) {
      uint8_t dummy;
      xQueueReceive(s_cdc_rx_queue, &dummy, 0);
      xQueueSend(s_cdc_rx_queue, &buf[i], 0);
    }
    process_cmd_byte((char)buf[i]);
  }
}

static void cdc_line_state_callback(int itf, cdcacm_event_t* event) {
  (void)itf;
  s_cdc_host_ready = event->line_state_changed_data.dtr;
  ESP_LOGI(TAG, "CDC-Host %s (DTR=%d)", s_cdc_host_ready ? "bereit" : "nicht bereit", s_cdc_host_ready);
}

void usb_manager_init(void) {
  s_cdc_rx_queue = xQueueCreate(CDC_RX_QUEUE_LEN, sizeof(uint8_t));

  // Geraete-Deskriptor bleibt NULL (Kconfig-Default reicht), der
  // Konfigurations-Deskriptor muss wegen HID von Hand kommen (s_config_desc
  // oben) - siehe docs/entscheidungen.md.
  tinyusb_config_t tusb_cfg = {
      .device_descriptor = NULL,
      .configuration_descriptor = s_config_desc,
  };
  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

  tinyusb_config_cdcacm_t cdc_cfg = {
      .usb_dev = TINYUSB_USBDEV_0,
      .cdc_port = TINYUSB_CDC_ACM_0,
      .callback_rx = &cdc_rx_callback,
      .callback_rx_wanted_char = NULL,
      .callback_line_state_changed = &cdc_line_state_callback,
      .callback_line_coding_changed = NULL,
  };
  ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc_cfg));

  ESP_LOGI(TAG, "UsbManager gestartet (CDC + HID-Tastatur, Kommandoprotokoll " CMD_PREFIX ")");
}

bool usb_manager_cdc_host_ready(void) { return s_cdc_host_ready; }

void usb_manager_cdc_write(const uint8_t* data, size_t len) {
  tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
  tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

QueueHandle_t usb_manager_get_cdc_rx_queue(void) { return s_cdc_rx_queue; }

void usb_manager_send_key(uint8_t modifier, uint8_t keycode) {
  if (!tud_hid_n_ready(0)) return;
  uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
  tud_hid_n_keyboard_report(0, 0, modifier, keycodes);
  // Kurze Verzoegerung, damit der Host den Tastendruck als eigenes Ereignis
  // erkennt, dann Release-Report (alles 0) senden.
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t released[6] = {0};
  tud_hid_n_keyboard_report(0, 0, 0, released);
}

// ---------------------------------------------------------------------
// TinyUSB-HID-Pflichtkallbacks (kein Weak-Default in dieser TinyUSB-Version,
// siehe managed_components/espressif__tinyusb/src/class/hid/hid_device.c -
// ohne diese drei schlaegt der Link fehl).
// ---------------------------------------------------------------------

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer,
                                uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;  // GET_REPORT wird nicht unterstuetzt
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer,
                            uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
  // SET_REPORT (z.B. Tastatur-LEDs) wird nicht ausgewertet.
}
