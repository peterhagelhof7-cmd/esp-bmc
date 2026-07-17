#include "web_server_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_log.h"
#include "config_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_manager.h"
#include "login_html.h"
#include "network_manager.h"
#include "sensor_history.h"
#include "sensor_manager.h"
#include "usb_manager.h"
#include "user_manager.h"
#include "wireguard_manager.h"

static const char* TAG = "web_server_manager";

static httpd_handle_t s_server;
static int s_ws_console_fd = -1;

// ---------------------------------------------------------------------
// Hilfsfunktionen: Cookie-/Formular-Parsing
// ---------------------------------------------------------------------

static char hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// Dekodiert x-www-form-urlencoded ("+"->Leerzeichen, "%XX"->Byte) in-place-
// artig von src nach dst (dst darf gleich src sein, ist immer <= Laenge).
static void url_decode(const char* src, char* dst, size_t dst_len) {
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0' && o + 1 < dst_len; i++) {
    if (src[i] == '+') {
      dst[o++] = ' ';
    } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      dst[o++] = (char)((hex_nibble(src[i + 1]) << 4) | hex_nibble(src[i + 2]));
      i += 2;
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = '\0';
}

// Sucht "key=..." in einem "a=1&b=2"-Formularkoerper und schreibt den
// (URL-dekodierten) Wert nach out.
static void parse_form_field(const char* body, const char* key, char* out, size_t out_len) {
  out[0] = '\0';
  size_t key_len = strlen(key);
  const char* p = body;
  while (p) {
    if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
      const char* value_start = p + key_len + 1;
      const char* amp = strchr(value_start, '&');
      char raw[700];  // gross genug fuer eine eingefuegte wireguard.conf (Feld "conf")
      size_t raw_len = amp ? (size_t)(amp - value_start) : strlen(value_start);
      if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
      memcpy(raw, value_start, raw_len);
      raw[raw_len] = '\0';
      url_decode(raw, out, out_len);
      return;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
}

// Liest das Session-Cookie aus dem Request und validiert es. true, wenn
// eine gueltige Session vorliegt.
static bool get_session(httpd_req_t* req, char out_username[32], user_role_t* out_role) {
  char cookie_hdr[128];
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) return false;

  const char* pos = strstr(cookie_hdr, "session=");
  if (!pos) return false;
  pos += strlen("session=");

  char token[64];
  size_t i = 0;
  while (pos[i] != '\0' && pos[i] != ';' && i + 1 < sizeof(token)) {
    token[i] = pos[i];
    i++;
  }
  token[i] = '\0';

  return user_manager_session_validate(token, out_username, out_role);
}

static void redirect_to(httpd_req_t* req, const char* location) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_send(req, NULL, 0);
}

static const char* role_name(user_role_t role) {
  switch (role) {
    case USER_ROLE_ADMIN: return "Admin";
    case USER_ROLE_VERWALTER: return "Verwalter";
    case USER_ROLE_SSH_USER: return "SSH User";
    default: return "Leser";
  }
}

// Session validieren UND Mindestrolle pruefen (webconfig.txt: die
// Einstellungen-Seite ist Verwaltern und Admins vorbehalten). Bei
// fehlender Session -> Redirect zum Login, bei zu niedriger Rolle -> 403.
// Liefert true, wenn der Request weiterbehandelt werden darf.
static bool require_role(httpd_req_t* req, user_role_t min_role, char out_username[32], user_role_t* out_role) {
  if (!get_session(req, out_username, out_role)) {
    redirect_to(req, "/login");
    return false;
  }
  if (*out_role < min_role) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req, "Keine Berechtigung fuer diese Seite.", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------
// HTTP-Handler
// ---------------------------------------------------------------------

static esp_err_t login_get_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, LOGIN_HTML, sizeof(LOGIN_HTML) - 1);
  return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t* req) {
  char body[256] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char username[32], password[64];
  parse_form_field(body, "username", username, sizeof(username));
  parse_form_field(body, "password", password, sizeof(password));

  user_role_t role;
  if (user_manager_authenticate(username, password, &role)) {
    char token[33];
    user_manager_session_create(username, role, token);
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    redirect_to(req, "/");
    ESP_LOGI(TAG, "Login erfolgreich: %s (%s)", username, role_name(role));
    char event[64];
    snprintf(event, sizeof(event), "Login (Web): %s (%s)", username, role_name(role));
    audit_log_add(event);
  } else {
    redirect_to(req, "/login?failed=1");
    ESP_LOGW(TAG, "Login fehlgeschlagen fuer Benutzername \"%s\"", username);
  }
  return ESP_OK;
}

static esp_err_t logout_get_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  char cookie_hdr[128];
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) == ESP_OK) {
    const char* pos = strstr(cookie_hdr, "session=");
    if (pos) {
      pos += strlen("session=");
      char token[64];
      size_t i = 0;
      while (pos[i] != '\0' && pos[i] != ';' && i + 1 < sizeof(token)) {
        token[i] = pos[i];
        i++;
      }
      token[i] = '\0';
      user_manager_session_invalidate(token);
    }
  }
  (void)username;
  (void)role;
  httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; HttpOnly; Max-Age=0");
  redirect_to(req, "/login");
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!get_session(req, username, &role)) {
    redirect_to(req, "/login");
    return ESP_OK;
  }

  char ip[16] = "-";
  network_manager_get_ip_string(ip, sizeof(ip));

  float ntc_temp = 0, dht_temp = 0, dht_hum = 0;
  bool ntc_ok = sensor_manager_get_ntc_temp_c(&ntc_temp);
  bool dht_ok = sensor_manager_get_dht_temp_c(&dht_temp) && sensor_manager_get_dht_humidity_pct(&dht_hum);

  int64_t uptime_s = esp_timer_get_time() / 1000000;

  bool power_led_on = gpio_manager_read_power_led();
  bool hdd_led_recent = gpio_manager_hdd_led_active_recently();

  char ntc_str[32];
  if (ntc_ok) {
    snprintf(ntc_str, sizeof(ntc_str), "%.1f &deg;C", ntc_temp);
  } else {
    snprintf(ntc_str, sizeof(ntc_str), "kein gueltiger Messwert");
  }
  char dht_str[64];
  if (dht_ok) {
    snprintf(dht_str, sizeof(dht_str), "%.1f &deg;C, %.1f %% rH", dht_temp, dht_hum);
  } else {
    snprintf(dht_str, sizeof(dht_str), "kein gueltiger Messwert");
  }

  char wg_local_ip[16];
  char wg_endpoint[80];
  wireguard_manager_get_local_address(wg_local_ip, sizeof(wg_local_ip));
  wireguard_manager_get_endpoint(wg_endpoint, sizeof(wg_endpoint));
  bool wg_up = wireguard_manager_is_up();

  char final_page[3200];
  snprintf(final_page, sizeof(final_page),
           "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<title>ESP-BMC - Uebersicht</title>"
           "<style>"
           "body{font-family:sans-serif;background:#f2f0e9;margin:0;padding:1.5rem;color:#1c2430;}"
           "h1{font-size:1.2rem;}"
           ".card{background:#fff;border:1px solid #e4e1d8;border-radius:6px;padding:1rem;"
           "margin-bottom:0.8rem;}"
           ".led{display:inline-block;width:1.2rem;height:1.2rem;border-radius:50%%;"
           "vertical-align:middle;margin-right:0.4rem;}"
           ".on{background:#3f7a4d;}.off{background:#ccc;}"
           ".vpn-up{background:#3f7a4d;}.vpn-down{background:#a63d2e;}"
           ".hdd-on{background:#a63d2e;animation:hddblink 1s infinite;}.hdd-off{background:#ccc;}"
           "@keyframes hddblink{50%%{opacity:0.25;}}"
           "canvas{max-width:100%%;background:#fbfaf7;border:1px solid #e4e1d8;border-radius:6px;}"
           "a{color:#0f1f3d;}"
           "</style></head><body>"
           "<h1>ESP-BMC &mdash; Uebersicht</h1>"
           "<p>Angemeldet als <b>%s</b> (%s) &middot; <a href=\"/logout\">Abmelden</a></p>"
           "<div class=\"card\"><b>Netzwerk</b><br>WLAN-IP: %s<br>"
           "VPN-IP: %s &middot; Ziel: %s<br>"
           "VPN-Status: <span class=\"led %s\"></span>%s"
           "</div>"
           "<div class=\"card\"><b>Sensorik</b><br>"
           "NTC-Temperatur: %s<br>DHT11: %s"
           "</div>"
           "<div class=\"card\"><b>Sensorwerte, 24h</b><br>"
           "<canvas id=\"chart\" height=\"200\"></canvas></div>"
           "<div class=\"card\"><b>System</b><br>Uptime: %lld s<br>"
           "Power-LED: <span class=\"led %s\"></span>%s<br>"
           "HDD-Aktivitaet (letzte 10s): <span class=\"led %s\"></span>%s"
           "</div>"
           "<div class=\"card\"><b>Webconsole</b><br>"
           "<a href=\"/console\">Zur interaktiven Konsole</a></div>"
           "<div class=\"card\"><b>Einstellungen</b><br>"
           "<a href=\"/settings\">Zur Einstellungen-Seite</a></div>"
           "<div class=\"card\"><b>Logs</b><br>"
           "<a href=\"/logs\">Zur Logs-Seite</a></div>"
           "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><script>"
           "fetch('/api/graph').then(r=>r.json()).then(d=>{"
           "new Chart(document.getElementById('chart'),{type:'line',data:{labels:d.labels,datasets:["
           "{label:'NTC (C)',data:d.ntc_temp,borderColor:'#a63d2e',yAxisID:'y'},"
           "{label:'DHT11 (C)',data:d.dht_temp,borderColor:'#c98a3a',yAxisID:'y'},"
           "{label:'DHT11 (%% rH)',data:d.dht_humidity,borderColor:'#2a5ba0',yAxisID:'y1'}]},"
           "options:{scales:{y:{position:'left'},y1:{position:'right',grid:{drawOnChartArea:false}}}}});});"
           "</script>"
           "</body></html>",
           username, role_name(role), ip, wg_local_ip, wg_endpoint, wg_up ? "vpn-up" : "vpn-down",
           wg_up ? "verbunden" : "getrennt", ntc_str, dht_str, (long long)uptime_s, power_led_on ? "on" : "off",
           power_led_on ? "aktiv" : "inaktiv", hdd_led_recent ? "hdd-on" : "hdd-off",
           hdd_led_recent ? "aktiv" : "inaktiv");

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, final_page, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ---------------------------------------------------------------------
// Einstellungen-Seite (webconfig.txt "Seite Einstellungen") - Verwalter
// und Admin. Reset (nur Einstellungen / Einstellungen+Werte) ist bewusst
// noch NICHT umgesetzt - "Werte" (Sensor-Historie) existiert als Feature
// noch gar nicht, siehe Projekt-Memory.
// ---------------------------------------------------------------------

// 24h-Sensorverlauf als JSON fuer den Chart auf der Uebersichtsseite
// (webconfig.txt: "sensor werte in einem chart (eines fuer alle) 24h ...,
// stuendliche werte") - gleiches Muster wie das etablierte "/api/graph"
// der Sensormeter-Familie (Chart.js via CDN, fetch() laedt die Daten
// nach - im Gegensatz zum Rest dieser Web-UI, die bewusst ohne
// JS/AJAX auskommt, ist ein Chart ohne Client-JS praktisch nicht
// sinnvoll darstellbar).
static esp_err_t api_graph_get_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!get_session(req, username, &role)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
  }

  sensor_history_entry_t entries[24];
  size_t n = sensor_history_get_entries(entries, 24);
  int64_t now = esp_timer_get_time();

  char body[2048];
  size_t off = snprintf(body, sizeof(body), "{\"labels\":[");
  for (size_t i = 0; i < n && off < sizeof(body) - 32; i++) {
    long hours_ago = (long)((now - entries[i].recorded_at_us) / 3600000000LL);
    off += snprintf(body + off, sizeof(body) - off, "%s\"-%ldh\"", i == 0 ? "" : ",", hours_ago);
  }
  off += snprintf(body + off, sizeof(body) - off, "],\"ntc_temp\":[");
  for (size_t i = 0; i < n && off < sizeof(body) - 32; i++) {
    off += entries[i].ntc_valid
               ? snprintf(body + off, sizeof(body) - off, "%s%.1f", i == 0 ? "" : ",", entries[i].ntc_temp_c)
               : snprintf(body + off, sizeof(body) - off, "%snull", i == 0 ? "" : ",");
  }
  off += snprintf(body + off, sizeof(body) - off, "],\"dht_temp\":[");
  for (size_t i = 0; i < n && off < sizeof(body) - 32; i++) {
    off += entries[i].dht_valid
               ? snprintf(body + off, sizeof(body) - off, "%s%.1f", i == 0 ? "" : ",", entries[i].dht_temp_c)
               : snprintf(body + off, sizeof(body) - off, "%snull", i == 0 ? "" : ",");
  }
  off += snprintf(body + off, sizeof(body) - off, "],\"dht_humidity\":[");
  for (size_t i = 0; i < n && off < sizeof(body) - 32; i++) {
    off += entries[i].dht_valid
               ? snprintf(body + off, sizeof(body) - off, "%s%.1f", i == 0 ? "" : ",", entries[i].dht_humidity_pct)
               : snprintf(body + off, sizeof(body) - off, "%snull", i == 0 ? "" : ",");
  }
  snprintf(body + off, sizeof(body) - off, "]}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char ip[16] = "-";
  network_manager_get_ip_string(ip, sizeof(ip));
  bool is_static = network_manager_is_static_ip();
  char cur_ip[16] = "", cur_mask[16] = "", cur_gw[16] = "";
  if (is_static) network_manager_get_static_config(cur_ip, cur_mask, cur_gw);

  bool wg_uploaded = wireguard_manager_has_uploaded_config();
  char wg_local_ip[16];
  wireguard_manager_get_local_address(wg_local_ip, sizeof(wg_local_ip));

  // Optionaler WLAN-Scan ueber "?scan=1" - kein JavaScript/AJAX in dieser
  // Ausbaustufe, ein einfacher Seiten-Reload reicht.
  char scan_html[1024] = "";
  char query[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "scan=1")) {
    network_wifi_scan_result_t results[10];
    int n = network_manager_scan_wifi(results, 10);
    size_t off = 0;
    off += snprintf(scan_html + off, sizeof(scan_html) - off, "<ul>");
    for (int i = 0; i < n && off < sizeof(scan_html) - 100; i++) {
      off += snprintf(scan_html + off, sizeof(scan_html) - off, "<li>%s %s (%d dBm)</li>", results[i].ssid,
                       results[i].open ? "(offen, kein Passwort)" : "&#128274;", results[i].rssi);
    }
    off += snprintf(scan_html + off, sizeof(scan_html) - off, "</ul>");
  }

  // Benutzerliste
  char users_html[512] = "";
  size_t uoff = 0;
  uoff += snprintf(users_html + uoff, sizeof(users_html) - uoff, "<ul>");
  for (size_t i = 0; i < user_manager_count() && uoff < sizeof(users_html) - 80; i++) {
    char uname[32];
    user_role_t urole;
    if (user_manager_get_at(i, uname, &urole)) {
      uoff += snprintf(users_html + uoff, sizeof(users_html) - uoff, "<li>%s (%s)</li>", uname, role_name(urole));
    }
  }
  uoff += snprintf(users_html + uoff, sizeof(users_html) - uoff, "</ul>");

  // Verwalter muss die Ausfuehrung mit dem eigenen Passwort bestaetigen,
  // Admin nicht (webconfig.txt Rollenliste) - daher das Feld nur fuer
  // Verwalter einblenden.
  char taster_pw_html[160] = "";
  if (role == USER_ROLE_VERWALTER) {
    snprintf(taster_pw_html, sizeof(taster_pw_html),
             "<input type=\"password\" name=\"confirm_password\" placeholder=\"Passwort bestaetigen\" required "
             "style=\"width:auto;display:inline-block;\">");
  }

  char page[8192];
  snprintf(
      page, sizeof(page),
      "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>ESP-BMC - Einstellungen</title>"
      "<style>"
      "body{font-family:sans-serif;background:#f2f0e9;margin:0;padding:1.5rem;color:#1c2430;}"
      "h1{font-size:1.2rem;} h2{font-size:1rem;margin-top:0;}"
      ".card{background:#fff;border:1px solid #e4e1d8;border-radius:6px;padding:1rem;margin-bottom:0.8rem;}"
      "label{display:block;font-size:0.85rem;margin:0.4rem 0 0.15rem 0;}"
      "input,select,textarea{width:100%%;padding:0.4rem;box-sizing:border-box;border:1px solid #ccc;"
      "border-radius:4px;font-family:inherit;}"
      "button{margin-top:0.6rem;padding:0.5rem 1rem;background:#0f1f3d;color:#fff;border:none;"
      "border-radius:4px;cursor:pointer;}"
      "a{color:#0f1f3d;}"
      "</style></head><body>"
      "<h1>ESP-BMC &mdash; Einstellungen</h1>"
      "<p>Angemeldet als <b>%s</b> (%s) &middot; <a href=\"/\">Zur Uebersicht</a></p>"

      "<div class=\"card\"><h2>IP-Konfiguration (WLAN)</h2>"
      "<p>Aktuelle IP: %s (%s)</p>"
      "<form method=\"post\" action=\"/settings/network\">"
      "<label>Modus</label><select name=\"mode\">"
      "<option value=\"dhcp\"%s>DHCP</option><option value=\"static\"%s>Statisch</option></select>"
      "<label>IP-Adresse</label><input type=\"text\" name=\"ip\" value=\"%s\">"
      "<label>Netzmaske</label><input type=\"text\" name=\"netmask\" value=\"%s\">"
      "<label>Gateway</label><input type=\"text\" name=\"gateway\" value=\"%s\">"
      "<button type=\"submit\">Uebernehmen (mit Ping-Check)</button>"
      "</form></div>"

      "<div class=\"card\"><h2>WLAN-Scan</h2>"
      "<a href=\"/settings?scan=1\"><button type=\"button\">Scan starten</button></a>"
      "%s"
      "</div>"

      "<div class=\"card\"><h2>WireGuard-VPN</h2>"
      "<p>Konfiguration: %s &middot; Tunnel-IP: %s</p>"
      "<form method=\"post\" action=\"/settings/wireguard/upload\">"
      "<label>wireguard.conf einfuegen</label>"
      "<textarea name=\"conf\" rows=\"8\" placeholder=\"[Interface]&#10;PrivateKey = ...&#10;Address = "
      "10.0.0.2/24&#10;&#10;[Peer]&#10;PublicKey = ...&#10;Endpoint = beispiel.de:51820&#10;AllowedIPs = "
      "0.0.0.0/0\"></textarea>"
      "<button type=\"submit\">Hochladen &amp; verbinden</button>"
      "</form>"
      "<form method=\"post\" action=\"/settings/wireguard/delete\" onsubmit=\"return "
      "confirm('VPN-Konfiguration wirklich loeschen?');\">"
      "<button type=\"submit\">VPN-Konfiguration loeschen</button>"
      "</form></div>"

      "<div class=\"card\"><h2>Benutzerverwaltung</h2>"
      "%s"
      "<form method=\"post\" action=\"/settings/users\">"
      "<label>Benutzername (keine Sonderzeichen)</label><input type=\"text\" name=\"username\" required>"
      "<label>Passwort (mind. 8 Zeichen, 2 von 3: Gross-/Kleinbuchstaben/Zahlen)</label>"
      "<input type=\"password\" name=\"password\" required>"
      "<label>Rolle</label><select name=\"role\">"
      "<option value=\"0\">Leser</option><option value=\"1\">SSH User</option>"
      "<option value=\"2\">Verwalter</option><option value=\"3\">Admin</option></select>"
      "<button type=\"submit\">Benutzer anlegen</button>"
      "</form></div>"

      "<div class=\"card\"><h2>Taster-Steuerung</h2>"
      "<form method=\"post\" action=\"/settings/taster\" style=\"display:inline-block;margin-right:0.5rem;\">"
      "<input type=\"hidden\" name=\"action\" value=\"power_push\">%s"
      "<button type=\"submit\">Power (kurz)</button></form>"
      "<form method=\"post\" action=\"/settings/taster\" style=\"display:inline-block;margin-right:0.5rem;\" "
      "onsubmit=\"return confirm('Power lange gedrueckt halten (erzwungenes Abschalten)?');\">"
      "<input type=\"hidden\" name=\"action\" value=\"power_hold\">%s"
      "<button type=\"submit\">Power (lang, erzwungen)</button></form>"
      "<form method=\"post\" action=\"/settings/taster\" style=\"display:inline-block;\">"
      "<input type=\"hidden\" name=\"action\" value=\"reset\">%s"
      "<button type=\"submit\">Reset</button></form>"
      "</div>"

      "<div class=\"card\"><h2>System</h2>"
      "<p><a href=\"/settings/config-download\">Gesamtkonfiguration herunterladen</a> &middot; "
      "<a href=\"/logs\">Zur Logs-Seite</a></p>"
      "<form method=\"post\" action=\"/settings/reboot\" onsubmit=\"return confirm('Geraet wirklich neu "
      "starten?');\">"
      "<button type=\"submit\">Neu starten</button>"
      "</form>"
      "<form method=\"post\" action=\"/settings/reset\" onsubmit=\"return confirm('Nur Einstellungen "
      "zuruecksetzen (WLAN/VPN/Benutzer/Schwellwerte) und neu starten?');\">"
      "<input type=\"hidden\" name=\"scope\" value=\"settings\">"
      "<button type=\"submit\">Reset: nur Einstellungen</button>"
      "</form>"
      "<form method=\"post\" action=\"/settings/reset\" onsubmit=\"return confirm('Einstellungen UND "
      "24h-Sensor-Historie zuruecksetzen und neu starten?');\">"
      "<input type=\"hidden\" name=\"scope\" value=\"settings_values\">"
      "<button type=\"submit\">Reset: Einstellungen + Werte</button>"
      "</form></div>"
      "</body></html>",
      username, role_name(role), ip, is_static ? "statisch" : "DHCP", is_static ? "" : " selected",
      is_static ? " selected" : "", cur_ip, cur_mask, cur_gw, scan_html,
      wg_uploaded ? "hochgeladen" : "Kconfig-Platzhalter", wg_local_ip, users_html, taster_pw_html, taster_pw_html,
      taster_pw_html);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t settings_network_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char body[256] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char mode[8], ip[16], netmask[16], gateway[16];
  parse_form_field(body, "mode", mode, sizeof(mode));
  parse_form_field(body, "ip", ip, sizeof(ip));
  parse_form_field(body, "netmask", netmask, sizeof(netmask));
  parse_form_field(body, "gateway", gateway, sizeof(gateway));

  if (strcmp(mode, "dhcp") == 0) {
    network_manager_use_dhcp();
    ESP_LOGI(TAG, "%s hat auf DHCP umgestellt", username);
    redirect_to(req, "/settings");
    return ESP_OK;
  }

  bool ok = network_manager_apply_static_ip(ip, netmask, gateway);
  ESP_LOGI(TAG, "%s hat statische IP %s versucht: %s", username, ip, ok ? "erfolgreich" : "Ping-Check fehlgeschlagen");
  redirect_to(req, ok ? "/settings" : "/settings?failed=network");
  return ESP_OK;
}

static esp_err_t settings_wireguard_upload_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char body[900] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char conf[700];
  parse_form_field(body, "conf", conf, sizeof(conf));

  esp_err_t err = wireguard_manager_apply_uploaded_config(conf);
  ESP_LOGI(TAG, "%s hat eine WireGuard-Konfiguration hochgeladen: %s", username,
           err == ESP_OK ? "erfolgreich" : "fehlgeschlagen");
  redirect_to(req, err == ESP_OK ? "/settings" : "/settings?failed=wireguard");
  return ESP_OK;
}

static esp_err_t settings_wireguard_delete_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  wireguard_manager_delete_config();
  ESP_LOGI(TAG, "%s hat die WireGuard-Konfiguration geloescht", username);
  redirect_to(req, "/settings");
  return ESP_OK;
}

static esp_err_t settings_users_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char body[256] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char new_username[32], new_password[64], role_str[4];
  parse_form_field(body, "username", new_username, sizeof(new_username));
  parse_form_field(body, "password", new_password, sizeof(new_password));
  parse_form_field(body, "role", role_str, sizeof(role_str));

  bool ok = user_manager_create(new_username, new_password, (user_role_t)atoi(role_str));
  ESP_LOGI(TAG, "%s hat Benutzer \"%s\" angelegt: %s", username, new_username, ok ? "erfolgreich" : "fehlgeschlagen");
  redirect_to(req, ok ? "/settings" : "/settings?failed=user");
  return ESP_OK;
}

// Taster-Steuerung (webconfig.txt Rollenliste: "steuerung der taster (power
// und reset, tastschutz)" - Admin ohne Ruecksicherung, Verwalter "muss mit
// dem eigenen passwort bestaetigt werden"). Wirkt ueber gpio_manager_trigger_*
// genauso wie ein physischer Tastendruck und respektiert damit denselben
// Tastschutz.
static esp_err_t settings_taster_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char body[192] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char action[16], confirm_password[64];
  parse_form_field(body, "action", action, sizeof(action));
  parse_form_field(body, "confirm_password", confirm_password, sizeof(confirm_password));

  // Verwalter muss die Ausfuehrung mit dem eigenen Passwort erneut
  // bestaetigen, Admin nicht (webconfig.txt Rollenliste).
  if (role == USER_ROLE_VERWALTER) {
    user_role_t tmp;
    if (!user_manager_authenticate(username, confirm_password, &tmp)) {
      ESP_LOGW(TAG, "%s: Taster-Steuerung ohne gueltige Passwort-Bestaetigung abgelehnt", username);
      redirect_to(req, "/settings?failed=taster");
      return ESP_OK;
    }
  }

  bool ok;
  const char* label;
  if (strcmp(action, "power_push") == 0) {
    ok = gpio_manager_trigger_power(false);
    label = "Power (kurz)";
  } else if (strcmp(action, "power_hold") == 0) {
    ok = gpio_manager_trigger_power(true);
    label = "Power (lang)";
  } else if (strcmp(action, "reset") == 0) {
    ok = gpio_manager_trigger_reset();
    label = "Reset";
  } else {
    redirect_to(req, "/settings?failed=taster");
    return ESP_OK;
  }

  char event[128];
  snprintf(event, sizeof(event), "Taster \"%s\" ausgeloest von %s (Web)%s", label, username,
           ok ? "" : " - durch Tastschutz blockiert");
  audit_log_add(event);
  ESP_LOGI(TAG, "%s", event);
  redirect_to(req, ok ? "/settings" : "/settings?failed=taster");
  return ESP_OK;
}

static esp_err_t settings_config_download_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char ip[16] = "-";
  network_manager_get_ip_string(ip, sizeof(ip));
  char wg_ip[16];
  wireguard_manager_get_local_address(wg_ip, sizeof(wg_ip));

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
  snprintf(body + off, sizeof(body) - off, "]}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"esp-bmc-config.json\"");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t settings_reboot_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  ESP_LOGW(TAG, "Neustart ausgeloest von Benutzer \"%s\"", username);
  char event[64];
  snprintf(event, sizeof(event), "Neustart ausgeloest von %s", username);
  audit_log_add(event);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, "Geraet startet neu...", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

// Reset (webconfig.txt "Seite Einstellungen": "reset (nur einstellungen,
// oder einstellungen und werte)"). "Einstellungen" = WLAN-Zugangsdaten,
// WireGuard-Konfiguration, Benutzerkonten (danach wieder nur
// admin/admin), Schwellwerte/Tastschutz. "Werte" = zusaetzlich die
// 24h-Sensor-Historie. Das Audit-Log selbst bleibt IMMER erhalten -
// sonst wuerde ein Reset seine eigene Spur verwischen, was dem Zweck
// eines Audit-Logs widerspraeche. Startet danach neu, damit alle
// betroffenen Komponenten (insbesondere WLAN/WireGuard) sauber mit den
// zurueckgesetzten Werten neu initialisieren.
static esp_err_t settings_reset_post_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char body[64] = {0};
  size_t to_read = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int received = httpd_req_recv(req, body, to_read);
  if (received <= 0) return ESP_FAIL;
  body[received] = '\0';

  char scope[24];
  parse_form_field(body, "scope", scope, sizeof(scope));
  bool include_values = (strcmp(scope, "settings_values") == 0);

  config_manager_reset_to_defaults();
  network_manager_reset();
  wireguard_manager_delete_config();
  user_manager_reset_to_default();
  if (include_values) sensor_history_reset();

  char event[112];
  snprintf(event, sizeof(event), "Reset (%s) ausgeloest von %s (Web)",
           include_values ? "Einstellungen+Werte" : "Einstellungen", username);
  audit_log_add(event);
  ESP_LOGW(TAG, "%s", event);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, "Zuruecksetzen abgeschlossen, Geraet startet neu...", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

// ---------------------------------------------------------------------
// Logs-Seite (webconfig.txt "Seite Logs") - Audit-Log (Verwalter+Admin,
// deckt sich mit den anderen Einstellungen-Berechtigungen) + Sensorwerte
// 24h (alle eingeloggten Rollen, "Leser = Log Download nur Sensorwerte").
// ---------------------------------------------------------------------

static esp_err_t logs_get_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_LESER, username, &role)) return ESP_OK;

  char page[1536];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<title>ESP-BMC - Logs</title>"
           "<style>"
           "body{font-family:sans-serif;background:#f2f0e9;margin:0;padding:1.5rem;color:#1c2430;}"
           "h1{font-size:1.2rem;} h2{font-size:1rem;margin-top:0;}"
           ".card{background:#fff;border:1px solid #e4e1d8;border-radius:6px;padding:1rem;margin-bottom:0.8rem;}"
           "a{color:#0f1f3d;}"
           "</style></head><body>"
           "<h1>ESP-BMC &mdash; Logs</h1>"
           "<p>Angemeldet als <b>%s</b> (%s) &middot; <a href=\"/\">Zur Uebersicht</a></p>"
           "<div class=\"card\"><h2>Sensorwerte (24h, stuendlich)</h2>"
           "<p><a href=\"/logs/sensors.csv\">Als CSV herunterladen</a></p></div>"
           "%s"
           "</body></html>",
           username, role_name(role),
           role >= USER_ROLE_VERWALTER
               ? "<div class=\"card\"><h2>Audit-Log</h2><p><a "
                 "href=\"/logs/audit.log\">Herunterladen (Verbindungen, Taster-Ereignisse)</a></p></div>"
               : "");

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t logs_sensors_csv_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_LESER, username, &role)) return ESP_OK;

  char csv[2048];
  size_t len = sensor_history_get_csv(csv, sizeof(csv));

  httpd_resp_set_type(req, "text/csv");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sensor-history-24h.csv\"");
  httpd_resp_send(req, csv, len);
  return ESP_OK;
}

static esp_err_t logs_audit_download_handler(httpd_req_t* req) {
  char username[32];
  user_role_t role;
  if (!require_role(req, USER_ROLE_VERWALTER, username, &role)) return ESP_OK;

  char content[4096];
  size_t len = audit_log_read(content, sizeof(content));

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"audit.log\"");
  httpd_resp_send(req, content, len);
  return ESP_OK;
}

// ---------------------------------------------------------------------
// WebSocket-Konsole - gebrueckt auf usb_manager's CDC-Queue (Pflichtenheft
// 3.6 "WebSocket-Handler fuer serielle Konsole").
// ---------------------------------------------------------------------

static esp_err_t ws_console_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    s_ws_console_fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "WebSocket-Konsole verbunden (fd=%d)", s_ws_console_fd);
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt = {0};
  uint8_t buf[128];
  ws_pkt.payload = buf;
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
  if (ret != ESP_OK) return ret;

  if (ws_pkt.len > 0 && ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
    usb_manager_cdc_write(ws_pkt.payload, ws_pkt.len);
  }
  return ESP_OK;
}

// Pumpt eingehende CDC-Rohbytes (aus usb_manager's Queue) an den zuletzt
// verbundenen WebSocket-Client weiter. Bewusst nur ein Client gleichzeitig -
// mehrere gleichzeitige Konsolen-Sitzungen sind fuer diese erste
// Ausbaustufe nicht vorgesehen.
static void console_pump_task(void* arg) {
  (void)arg;
  QueueHandle_t rx_queue = usb_manager_get_cdc_rx_queue();
  uint8_t buf[128];

  for (;;) {
    size_t n = 0;
    uint8_t byte;
    while (n < sizeof(buf) && xQueueReceive(rx_queue, &byte, n == 0 ? pdMS_TO_TICKS(200) : 0) == pdTRUE) {
      buf[n++] = byte;
    }
    if (n > 0 && s_ws_console_fd >= 0) {
      httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = buf, .len = n};
      httpd_ws_send_frame_async(s_server, s_ws_console_fd, &frame);
    }
  }
}

// ---------------------------------------------------------------------

void web_server_manager_init(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 20;  // Default (8) reicht seit Einstellungen+Logs nicht mehr

  ESP_ERROR_CHECK(httpd_start(&s_server, &config));

  httpd_uri_t login_get = {.uri = "/login", .method = HTTP_GET, .handler = login_get_handler};
  httpd_uri_t login_post = {.uri = "/login", .method = HTTP_POST, .handler = login_post_handler};
  httpd_uri_t logout_get = {.uri = "/logout", .method = HTTP_GET, .handler = logout_get_handler};
  httpd_uri_t root_get = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
  httpd_uri_t api_graph_get = {.uri = "/api/graph", .method = HTTP_GET, .handler = api_graph_get_handler};
  httpd_uri_t settings_get = {.uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler};
  httpd_uri_t settings_network_post = {
      .uri = "/settings/network", .method = HTTP_POST, .handler = settings_network_post_handler};
  httpd_uri_t settings_wg_upload_post = {
      .uri = "/settings/wireguard/upload", .method = HTTP_POST, .handler = settings_wireguard_upload_post_handler};
  httpd_uri_t settings_wg_delete_post = {
      .uri = "/settings/wireguard/delete", .method = HTTP_POST, .handler = settings_wireguard_delete_post_handler};
  httpd_uri_t settings_users_post = {
      .uri = "/settings/users", .method = HTTP_POST, .handler = settings_users_post_handler};
  httpd_uri_t settings_config_download = {
      .uri = "/settings/config-download", .method = HTTP_GET, .handler = settings_config_download_handler};
  httpd_uri_t settings_reboot_post = {
      .uri = "/settings/reboot", .method = HTTP_POST, .handler = settings_reboot_post_handler};
  httpd_uri_t settings_taster_post = {
      .uri = "/settings/taster", .method = HTTP_POST, .handler = settings_taster_post_handler};
  httpd_uri_t settings_reset_post = {
      .uri = "/settings/reset", .method = HTTP_POST, .handler = settings_reset_post_handler};
  httpd_uri_t logs_get = {.uri = "/logs", .method = HTTP_GET, .handler = logs_get_handler};
  httpd_uri_t logs_sensors_csv = {
      .uri = "/logs/sensors.csv", .method = HTTP_GET, .handler = logs_sensors_csv_handler};
  httpd_uri_t logs_audit_download = {
      .uri = "/logs/audit.log", .method = HTTP_GET, .handler = logs_audit_download_handler};
  httpd_uri_t ws_console = {
      .uri = "/ws/console", .method = HTTP_GET, .handler = ws_console_handler, .is_websocket = true};

  httpd_register_uri_handler(s_server, &login_get);
  httpd_register_uri_handler(s_server, &login_post);
  httpd_register_uri_handler(s_server, &logout_get);
  httpd_register_uri_handler(s_server, &root_get);
  httpd_register_uri_handler(s_server, &api_graph_get);
  httpd_register_uri_handler(s_server, &settings_get);
  httpd_register_uri_handler(s_server, &settings_network_post);
  httpd_register_uri_handler(s_server, &settings_wg_upload_post);
  httpd_register_uri_handler(s_server, &settings_wg_delete_post);
  httpd_register_uri_handler(s_server, &settings_users_post);
  httpd_register_uri_handler(s_server, &settings_config_download);
  httpd_register_uri_handler(s_server, &settings_reboot_post);
  httpd_register_uri_handler(s_server, &settings_taster_post);
  httpd_register_uri_handler(s_server, &settings_reset_post);
  httpd_register_uri_handler(s_server, &logs_get);
  httpd_register_uri_handler(s_server, &logs_sensors_csv);
  httpd_register_uri_handler(s_server, &logs_audit_download);
  httpd_register_uri_handler(s_server, &ws_console);

  xTaskCreate(console_pump_task, "console_pump", 3072, NULL, 1, NULL);

  ESP_LOGI(TAG, "WebServerManager gestartet (Port 80, Login+Uebersicht+Einstellungen+Logs+Webconsole)");
}
