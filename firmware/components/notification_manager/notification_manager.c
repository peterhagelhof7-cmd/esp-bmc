#include "notification_manager.h"

#include <stdio.h>
#include <string.h>

#include "audit_log.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "user_manager.h"

static const char* TAG = "notification_manager";

#define NOTIFY_CONFIG_FILE "/storage/notify.json"
#define HOST_CAP 64
#define SMTP_SENDER_CAP 64
#define SMTP_USERNAME_CAP 64
#define SMTP_PASSWORD_CAP 64
#define SOCKET_TIMEOUT_S 5  // fuer SMTP-Connect/Send/Recv - Syslog ist ein einzelnes UDP-Paket, kein Timeout noetig

static char s_syslog_server[HOST_CAP] = "";
static uint16_t s_syslog_port = 514;

static char s_smtp_server[HOST_CAP] = "";
static uint16_t s_smtp_port = 25;
static char s_smtp_sender[SMTP_SENDER_CAP] = "";
static char s_smtp_username[SMTP_USERNAME_CAP] = "";
static char s_smtp_password[SMTP_PASSWORD_CAP] = "";

// =============================================================================
// E-Mail-Entprellung - viele SMTP-Server/Relays begrenzen auf ca. 10
// Mails/Stunde. Ohne Bremse wuerde jede einzelne Schwellwert-Meldung
// (auch nach der Flankentriggerung in sensor_manager.c koennen mehrere
// verschiedene Messgroessen kurz hintereinander auffaellig werden) sofort
// eine eigene Mail ausloesen. Zaehler+Zeitfenster (Nutzervorgabe): die
// ersten 4 Mails eines Fensters gehen sofort raus, ab der 5. Mail
// innerhalb der ersten 50 Minuten des Fensters wird stattdessen
// gesammelt ("Digest") und eine einzige Sammel-Mail in Minute 59
// verschickt (max. 5 Mails/Fenster: 4 sofort + 1 Digest). Kommt die 5.
// Mail erst nach Minute 50, ist das Fenster ohnehin fast vorbei -> ganz
// normal sofort senden, kein Sammeln noetig.
// =============================================================================

#define BURST_IMMEDIATE_LIMIT 4      // so viele Mails gehen pro Fenster sofort raus
#define BURST_WINDOW_MIN 50          // ... sofern die 5. noch VOR dieser Minute noetig wird
#define DIGEST_FLUSH_MIN 59          // Sammel-Mail wird spaetestens in dieser Minute verschickt
#define WINDOW_LENGTH_MIN 60         // danach beginnt ein komplett neues Fenster
#define DIGEST_CHECK_INTERVAL_MS (30 * 1000)  // Polling-Intervall der Flush-Check-Task
#define DIGEST_BUFFER_CAP 1024

static int64_t s_window_start_us = 0;  // 0 = kein aktives Fenster
static int s_emails_sent_in_window = 0;
static bool s_collecting = false;
static char s_digest_buffer[DIGEST_BUFFER_CAP] = "";
static size_t s_digest_len = 0;
static unsigned s_digest_suppressed = 0;  // Meldungen, die aus Platzmangel nicht mehr in den Digest passten

// =============================================================================
// Persistenz (gleiches JSON-auf-storage-Muster wie config_manager/
// snmp_manager) - eigene Datei statt device.json, da eigenstaendiger
// Themenbereich mit eigenem Reset-Zeitpunkt.
// =============================================================================

static void save_config(void) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "syslog_server", s_syslog_server);
  cJSON_AddNumberToObject(root, "syslog_port", s_syslog_port);
  cJSON_AddStringToObject(root, "smtp_server", s_smtp_server);
  cJSON_AddNumberToObject(root, "smtp_port", s_smtp_port);
  cJSON_AddStringToObject(root, "smtp_sender", s_smtp_sender);
  cJSON_AddStringToObject(root, "smtp_username", s_smtp_username);
  cJSON_AddStringToObject(root, "smtp_password", s_smtp_password);
  char* text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) return;

  FILE* f = fopen(NOTIFY_CONFIG_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  } else {
    ESP_LOGE(TAG, "Konnte %s nicht schreiben", NOTIFY_CONFIG_FILE);
  }
  cJSON_free(text);
}

static void load_config(void) {
  FILE* f = fopen(NOTIFY_CONFIG_FILE, "r");
  if (!f) return;

  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* root = cJSON_Parse(buf);
  if (!root) return;

  cJSON* item;
  if ((item = cJSON_GetObjectItem(root, "syslog_server")) && cJSON_IsString(item)) {
    strncpy(s_syslog_server, item->valuestring, sizeof(s_syslog_server) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "syslog_port")) && cJSON_IsNumber(item)) {
    s_syslog_port = (uint16_t)item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "smtp_server")) && cJSON_IsString(item)) {
    strncpy(s_smtp_server, item->valuestring, sizeof(s_smtp_server) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "smtp_port")) && cJSON_IsNumber(item)) {
    s_smtp_port = (uint16_t)item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "smtp_sender")) && cJSON_IsString(item)) {
    strncpy(s_smtp_sender, item->valuestring, sizeof(s_smtp_sender) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "smtp_username")) && cJSON_IsString(item)) {
    strncpy(s_smtp_username, item->valuestring, sizeof(s_smtp_username) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "smtp_password")) && cJSON_IsString(item)) {
    strncpy(s_smtp_password, item->valuestring, sizeof(s_smtp_password) - 1);
  }
  cJSON_Delete(root);
}

static void digest_flush_task(void* arg);  // Definition weiter unten, bei der Entprellungs-Logik

void notification_manager_init(void) {
  load_config();
  xTaskCreate(digest_flush_task, "notify_digest", 3072, NULL, 1, NULL);
  ESP_LOGI(TAG, "NotificationManager gestartet (Syslog: %s, SMTP: %s)", s_syslog_server[0] ? "konfiguriert" : "aus",
           s_smtp_server[0] ? "konfiguriert" : "aus");
}

bool notification_manager_set_syslog(const char* server, uint16_t port) {
  if (strlen(server) >= sizeof(s_syslog_server)) return false;
  strncpy(s_syslog_server, server, sizeof(s_syslog_server) - 1);
  s_syslog_server[sizeof(s_syslog_server) - 1] = '\0';
  s_syslog_port = port;
  save_config();
  return true;
}

void notification_manager_get_syslog(char* out_server, size_t out_len, uint16_t* out_port) {
  strncpy(out_server, s_syslog_server, out_len - 1);
  out_server[out_len - 1] = '\0';
  *out_port = s_syslog_port;
}

bool notification_manager_set_smtp(const char* server, uint16_t port, const char* sender, const char* username,
                                    const char* password) {
  if (strlen(server) >= sizeof(s_smtp_server) || strlen(sender) >= sizeof(s_smtp_sender) ||
      strlen(username) >= sizeof(s_smtp_username) || strlen(password) >= sizeof(s_smtp_password)) {
    return false;
  }
  strncpy(s_smtp_server, server, sizeof(s_smtp_server) - 1);
  s_smtp_server[sizeof(s_smtp_server) - 1] = '\0';
  s_smtp_port = port;
  strncpy(s_smtp_sender, sender, sizeof(s_smtp_sender) - 1);
  s_smtp_sender[sizeof(s_smtp_sender) - 1] = '\0';
  strncpy(s_smtp_username, username, sizeof(s_smtp_username) - 1);
  s_smtp_username[sizeof(s_smtp_username) - 1] = '\0';

  // Leeres Passwort im Formular = "unveraendert lassen" (siehe Header) -
  // das gespeicherte Passwort wird der Einstellungen-Seite nie
  // zurueckgegeben, also muss ein Nicht-Aendern-Wunsch als leeres Feld
  // ankommen, nicht als (unsichtbar vorausgefuelltes) echtes Passwort.
  if (password[0] != '\0') {
    strncpy(s_smtp_password, password, sizeof(s_smtp_password) - 1);
    s_smtp_password[sizeof(s_smtp_password) - 1] = '\0';
  }
  save_config();
  return true;
}

void notification_manager_get_smtp(char* out_server, size_t out_server_len, uint16_t* out_port, char* out_sender,
                                    size_t out_sender_len, char* out_username, size_t out_username_len) {
  strncpy(out_server, s_smtp_server, out_server_len - 1);
  out_server[out_server_len - 1] = '\0';
  *out_port = s_smtp_port;
  strncpy(out_sender, s_smtp_sender, out_sender_len - 1);
  out_sender[out_sender_len - 1] = '\0';
  strncpy(out_username, s_smtp_username, out_username_len - 1);
  out_username[out_username_len - 1] = '\0';
  // Passwort wird bewusst NICHT zurueckgegeben, siehe Header-Kommentar.
}

void notification_manager_reset_to_defaults(void) {
  s_syslog_server[0] = '\0';
  s_syslog_port = 514;
  s_smtp_server[0] = '\0';
  s_smtp_port = 25;
  s_smtp_sender[0] = '\0';
  s_smtp_username[0] = '\0';
  s_smtp_password[0] = '\0';
  remove(NOTIFY_CONFIG_FILE);

  s_window_start_us = 0;
  s_emails_sent_in_window = 0;
  s_collecting = false;
  s_digest_buffer[0] = '\0';
  s_digest_len = 0;
  s_digest_suppressed = 0;
}

// =============================================================================
// DNS-Aufloesung - gemeinsam fuer Syslog (UDP) und SMTP (TCP), akzeptiert
// sowohl Hostnamen als auch rohe IP-Adressen (getaddrinfo deckt beides ab).
// =============================================================================

static bool resolve_host(const char* host, uint16_t port, struct sockaddr_in* out) {
  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  struct addrinfo* res = NULL;
  if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;

  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(port);
  out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

// =============================================================================
// Syslog (UDP, an RFC 3164 angelehnt - nur PRI-Praefix + Hostname +
// Nachricht, kein Zeitstempel-Parsing noetig, der Empfaenger stempelt
// selbst beim Empfang).
// =============================================================================

static void send_syslog(const char* message) {
  if (s_syslog_server[0] == '\0') return;

  struct sockaddr_in addr;
  if (!resolve_host(s_syslog_server, s_syslog_port, &addr)) {
    ESP_LOGW(TAG, "Syslog-Server \"%s\" nicht aufloesbar", s_syslog_server);
    return;
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return;

  // PRI 12 = facility 1 (user-level) * 8 + severity 4 (warning) - passend
  // zu einem sich selbst heilenden Schwellwert-Alarm (kein harter Fehler).
  char packet[256];
  snprintf(packet, sizeof(packet), "<12>esp-bmc: %s", message);
  sendto(sock, packet, strlen(packet), 0, (struct sockaddr*)&addr, sizeof(addr));
  close(sock);
}

// =============================================================================
// SMTP - bewusst OHNE TLS (siehe notification_manager.h/entscheidungen.md
// fuer die Kosten/Nutzen-Abwaegung). Reiner Klartext-Protokollablauf ueber
// einen rohen TCP-Socket, kein externer Client noetig.
// =============================================================================

static const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
  size_t oi = 0, i = 0;
  for (; i + 3 <= in_len && oi + 4 < out_cap; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[oi++] = B64_TABLE[(v >> 18) & 0x3F];
    out[oi++] = B64_TABLE[(v >> 12) & 0x3F];
    out[oi++] = B64_TABLE[(v >> 6) & 0x3F];
    out[oi++] = B64_TABLE[v & 0x3F];
  }
  size_t rem = in_len - i;
  if (rem == 1 && oi + 4 < out_cap) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[oi++] = B64_TABLE[(v >> 18) & 0x3F];
    out[oi++] = B64_TABLE[(v >> 12) & 0x3F];
    out[oi++] = '=';
    out[oi++] = '=';
  } else if (rem == 2 && oi + 4 < out_cap) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[oi++] = B64_TABLE[(v >> 18) & 0x3F];
    out[oi++] = B64_TABLE[(v >> 12) & 0x3F];
    out[oi++] = B64_TABLE[(v >> 6) & 0x3F];
    out[oi++] = '=';
  }
  out[oi] = '\0';
}

static bool smtp_send_line(int sock, const char* line) {
  char buf[320];
  int len = snprintf(buf, sizeof(buf), "%s\r\n", line);
  return send(sock, buf, len, 0) == len;
}

// Liest eine (ggf. mehrzeilige) SMTP-Antwort byteweise und liefert den
// Code der LETZTEN Zeile ("250-erste\r\n250 letzte\r\n" -> 250).
// Mehrzeilige Antworten erkennt man am 4. Zeichen: '-' = es folgt noch
// eine Zeile, ' ' = letzte Zeile der Antwort.
static int smtp_read_response(int sock) {
  char line[300];
  int code = -1;
  for (;;) {
    size_t n = 0;
    for (;;) {
      char c;
      if (recv(sock, &c, 1, 0) <= 0) return -1;
      if (c == '\n') break;
      if (c == '\r') continue;
      if (n < sizeof(line) - 1) line[n++] = c;
    }
    line[n] = '\0';
    if (n < 4) return -1;
    code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (line[3] == ' ') break;
  }
  return code;
}

// Body kann mehrzeilig sein (Digest-Sammelmail, "\n"-getrennt) - jede
// Zeile einzeln mit korrektem CRLF senden, statt den ganzen Body als
// eine "Zeile" an smtp_send_line() zu uebergeben.
static bool smtp_send_body_lines(int sock, const char* body) {
  char line[320];
  const char* p = body;
  while (*p) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    if (!smtp_send_line(sock, line)) return false;
    if (!nl) break;
    p = nl + 1;
  }
  return true;
}

#define MAX_NOTIFY_RECIPIENTS 16  // entspricht user_manager's MAX_USERS-Obergrenze

// Genau EINE E-Mail fuer ALLE aktivierten Empfaenger (RCPT TO je
// Empfaenger auf derselben Verbindung, alle sichtbar im Cc-Header) -
// spart Mails gegenueber einer Mail pro Empfaenger, siehe
// docs/entscheidungen.md "Benachrichtigungswege". "To:" zeigt bewusst
// den Absender selbst (kein einzelner Haupt-Empfaenger vorgesehen), die
// eigentliche Empfaengerliste steht im Cc.
static bool smtp_send_all(const char* subject, const char* body) {
  if (s_smtp_server[0] == '\0' || s_smtp_sender[0] == '\0') return false;

  size_t count = user_manager_count_notify_recipients();
  if (count > MAX_NOTIFY_RECIPIENTS) count = MAX_NOTIFY_RECIPIENTS;
  if (count == 0) return false;

  char emails[MAX_NOTIFY_RECIPIENTS][64];
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (user_manager_get_notify_recipient_at(i, emails[n], sizeof(emails[n]))) n++;
  }
  if (n == 0) return false;

  struct sockaddr_in addr;
  if (!resolve_host(s_smtp_server, s_smtp_port, &addr)) {
    ESP_LOGW(TAG, "SMTP-Server \"%s\" nicht aufloesbar", s_smtp_server);
    return false;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return false;
  struct timeval tv = {.tv_sec = SOCKET_TIMEOUT_S, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  bool ok = false;
  char line[320];

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) goto done;
  if (smtp_read_response(sock) != 220) goto done;

  if (!smtp_send_line(sock, "EHLO esp-bmc") || smtp_read_response(sock) / 100 != 2) goto done;

  if (s_smtp_username[0] != '\0') {
    char b64[128];
    if (!smtp_send_line(sock, "AUTH LOGIN") || smtp_read_response(sock) != 334) goto done;
    base64_encode((const uint8_t*)s_smtp_username, strlen(s_smtp_username), b64, sizeof(b64));
    if (!smtp_send_line(sock, b64) || smtp_read_response(sock) != 334) goto done;
    base64_encode((const uint8_t*)s_smtp_password, strlen(s_smtp_password), b64, sizeof(b64));
    if (!smtp_send_line(sock, b64) || smtp_read_response(sock) != 235) goto done;
  }

  snprintf(line, sizeof(line), "MAIL FROM:<%s>", s_smtp_sender);
  if (!smtp_send_line(sock, line) || smtp_read_response(sock) != 250) goto done;

  // Ein RCPT TO je Empfaenger (Umschlag-Ebene - der Server liefert
  // danach aus, unabhaengig vom Cc-Header). Ein einzelner abgelehnter
  // Empfaenger bricht nicht den gesamten Versand ab, wird nur geloggt.
  for (size_t i = 0; i < n; i++) {
    snprintf(line, sizeof(line), "RCPT TO:<%s>", emails[i]);
    if (!smtp_send_line(sock, line) || smtp_read_response(sock) / 100 != 2) {
      ESP_LOGW(TAG, "RCPT TO <%s> abgelehnt", emails[i]);
    }
  }

  if (!smtp_send_line(sock, "DATA") || smtp_read_response(sock) != 354) goto done;

  snprintf(line, sizeof(line), "From: %s", s_smtp_sender);
  if (!smtp_send_line(sock, line)) goto done;
  snprintf(line, sizeof(line), "To: %s", s_smtp_sender);
  if (!smtp_send_line(sock, line)) goto done;
  {
    char cc_line[MAX_NOTIFY_RECIPIENTS * 66 + 8];
    size_t off = (size_t)snprintf(cc_line, sizeof(cc_line), "Cc: ");
    for (size_t i = 0; i < n && off < sizeof(cc_line); i++) {
      off += (size_t)snprintf(cc_line + off, sizeof(cc_line) - off, "%s%s", i == 0 ? "" : ", ", emails[i]);
    }
    if (!smtp_send_line(sock, cc_line)) goto done;
  }
  snprintf(line, sizeof(line), "Subject: %s", subject);
  if (!smtp_send_line(sock, line)) goto done;
  if (!smtp_send_line(sock, "")) goto done;  // Leerzeile trennt Header von Body
  if (!smtp_send_body_lines(sock, body)) goto done;
  if (!smtp_send_line(sock, ".")) goto done;
  if (smtp_read_response(sock) != 250) goto done;

  smtp_send_line(sock, "QUIT");
  ok = true;

done:
  close(sock);
  return ok;
}

// =============================================================================
// E-Mail-Entprellung - siehe Erklaerung beim Zustand oben.
// =============================================================================

static void reset_window(int64_t now) {
  s_window_start_us = now;
  s_emails_sent_in_window = 0;
  s_collecting = false;
  s_digest_buffer[0] = '\0';
  s_digest_len = 0;
  s_digest_suppressed = 0;
}

static void append_to_digest(const char* message) {
  size_t msg_len = strlen(message);
  // 40 Byte Reserve fuer den "... N weitere unterdrueckt"-Hinweis beim
  // Flush, falls der Puffer bis dahin randvoll ist.
  if (s_digest_len + msg_len + 2 >= sizeof(s_digest_buffer) - 40) {
    s_digest_suppressed++;
    return;
  }
  if (s_digest_len > 0) s_digest_buffer[s_digest_len++] = '\n';
  memcpy(s_digest_buffer + s_digest_len, message, msg_len);
  s_digest_len += msg_len;
  s_digest_buffer[s_digest_len] = '\0';
}

static void flush_digest(void) {
  if (s_digest_len == 0) {
    s_collecting = false;
    return;
  }
  char body[DIGEST_BUFFER_CAP + 64];
  if (s_digest_suppressed > 0) {
    snprintf(body, sizeof(body), "%s\n... (%u weitere Meldung(en) unterdrueckt)", s_digest_buffer,
             s_digest_suppressed);
  } else {
    snprintf(body, sizeof(body), "%s", s_digest_buffer);
  }

  bool ok = smtp_send_all("ESP-BMC: Sammel-Benachrichtigung", body);
  ESP_LOGI(TAG, "SMTP-Sammel-Benachrichtigung: %s", ok ? "gesendet" : "fehlgeschlagen");
  if (ok) s_emails_sent_in_window++;

  s_digest_buffer[0] = '\0';
  s_digest_len = 0;
  s_digest_suppressed = 0;
  s_collecting = false;
}

// Wird pro Alarm aufgerufen (nach Syslog, das keiner Bremse unterliegt -
// UDP-Feuer-und-vergessen ohne Server-Kontingent-Problem). Entscheidet,
// ob sofort gesendet, gesammelt, oder (falls schon gesammelt wird)
// zusaetzlich in den Digest aufgenommen wird.
static void notify_email(const char* message) {
  if (s_smtp_server[0] == '\0' || s_smtp_sender[0] == '\0') return;

  int64_t now = esp_timer_get_time();
  if (s_window_start_us == 0 || (now - s_window_start_us) >= (int64_t)WINDOW_LENGTH_MIN * 60 * 1000000) {
    reset_window(now);
  }
  int elapsed_min = (int)((now - s_window_start_us) / (60LL * 1000000));

  if (s_collecting) {
    append_to_digest(message);
    return;
  }

  if (s_emails_sent_in_window < BURST_IMMEDIATE_LIMIT) {
    bool ok = smtp_send_all("ESP-BMC: Schwellwert ueberschritten", message);
    ESP_LOGI(TAG, "SMTP-Benachrichtigung: %s", ok ? "gesendet" : "fehlgeschlagen");
    if (ok) s_emails_sent_in_window++;
    return;
  }

  if (elapsed_min < BURST_WINDOW_MIN) {
    // 5. Mail (oder mehr) innerhalb der ersten 50 Minuten des Fensters -
    // ab jetzt sammeln statt einzeln versenden.
    ESP_LOGW(TAG, "Zu viele Benachrichtigungen (%d in %d min) - sammle bis Minute %d", s_emails_sent_in_window,
             elapsed_min, DIGEST_FLUSH_MIN);
    s_collecting = true;
    append_to_digest(message);
    return;
  }

  // 5. Mail (oder mehr), aber erst nach Minute 50 - das Fenster ist
  // ohnehin fast vorbei, kein Sammeln noetig.
  bool ok = smtp_send_all("ESP-BMC: Schwellwert ueberschritten", message);
  ESP_LOGI(TAG, "SMTP-Benachrichtigung: %s", ok ? "gesendet" : "fehlgeschlagen");
  if (ok) s_emails_sent_in_window++;
}

// Timer-gekoppelter Zaehler (Nutzervorgabe: "counter mit einem timer
// koppeln") - reines Event-getriebenes Sammeln wuerde den Digest nie
// verschicken, falls nach dem Start des Sammelns kein weiterer Alarm
// mehr eintrifft. Diese Task prueft daher unabhaengig von neuen Alarmen
// periodisch, ob Minute 59 erreicht ist.
static void digest_flush_task(void* arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(DIGEST_CHECK_INTERVAL_MS));
    if (!s_collecting || s_window_start_us == 0) continue;
    int elapsed_min = (int)((esp_timer_get_time() - s_window_start_us) / (60LL * 1000000));
    if (elapsed_min >= DIGEST_FLUSH_MIN) flush_digest();
  }
}

// =============================================================================
// Aufrufpunkt (SensorManager, flankengetriggert - siehe sensor_manager.c)
// =============================================================================

void notification_manager_trigger(const char* quelle, float value, float threshold) {
  ESP_LOGW(TAG, "Schwellwert ueberschritten: %s = %.1f > %.1f", quelle, value, threshold);

  char message[128];
  snprintf(message, sizeof(message), "Schwellwert ueberschritten: %s = %.1f > %.1f", quelle, value, threshold);
  audit_log_add(message);

  send_syslog(message);
  notify_email(message);
}
