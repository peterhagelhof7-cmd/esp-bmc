#include "ssh_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "usb_manager.h"
#include "user_manager.h"

#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

static const char* TAG = "ssh_manager";

#define SSH_PORT 22
#define HOST_KEY_FILE "/storage/ssh_host_key.der"
#define HOST_KEY_BUF_CAP 256  // ECC-P256-Privatschluessel im DER-Format passt bequem hinein
#define SOCKET_TIMEOUT_MS 200  // fuer das Polling zwischen SSH-Handshake-Schritten/Channel-Worker
// Jede angenommene Verbindung laeuft in einer eigenen Task, damit eine haengende
// Sitzung NIE den accept()-Loop blockiert (fruehere Ursache fuer "Port 22 tot bis
// Reboot"). Cap=2 (statt 1) erlaubt die kurze Ueberlappung fuers Takeover: eine
// neue Sitzung darf sich verbinden, waehrend die alte noch abgebaut wird - die
// neue uebernimmt die (weiterhin einzige) Konsole per Generation-Claim, die alte
// erkennt den Verlust und beendet sich. Eine 3. gleichzeitige Verbindung wird
// weiterhin sofort geschlossen (nicht-blockierend, kein Wedge).
#define MAX_SSH_SESSIONS 2
// Deckel fuer tote/haengende Sitzungen, die sonst den (einzigen) Konsolen-Slot
// belegen wuerden - siehe Kommentar an der Timeout-Pruefung in handle_session().
#define SESSION_IDLE_TIMEOUT_MS (5 * 60 * 1000)
// Deckel fuer die Handshake-/Anmeldephase: ein mitten im Handshake getrennter
// Client (Scanner/Flood/Netzabriss) haelt sonst den Slot, bis TCP-Keepalive
// greift. Muss die interaktive Passwort-Eingabe (Mensch tippt, ggf. mit
// Fehlversuchen) abdecken - deshalb 60s statt weniger. Der eigene Session-Task
// (MAX_SSH_SESSIONS=1) sorgt dafuer, dass das den accept()-Loop nicht blockiert.
#define HANDSHAKE_TIMEOUT_MS (60 * 1000)

static WOLFSSH_CTX* s_ctx;
static byte s_host_key[HOST_KEY_BUF_CAP];
static word32 s_host_key_len;

#define SSH_KEY_TYPE "ecdsa-sha2-nistp256"
#define SSH_CURVE_NAME "nistp256"

static char s_fingerprint[64] = "";
static char s_public_key_line[300] = "";

// =============================================================================
// Host-Key: einmalig erzeugt (ECC P-256 - RSA ist projektweit deaktiviert,
// siehe firmware/CMakeLists.txt), auf der storage-Partition persistiert.
// Ohne das wuerde bei jedem Neustart ein neuer Host-Key entstehen und
// jeder SSH-Client wuerde bei jeder Verbindung vor einem vermeintlichen
// MITM-Angriff warnen.
// =============================================================================

static bool load_host_key(void) {
  FILE* f = fopen(HOST_KEY_FILE, "rb");
  if (!f) return false;
  size_t n = fread(s_host_key, 1, sizeof(s_host_key), f);
  fclose(f);
  if (n == 0) return false;
  s_host_key_len = (word32)n;
  return true;
}

// Nur beim allerersten Boot (kein persistierter Key vorhanden) - reine
// Software-ECC (NO_ESP32_CRYPT), fuer P-256 aber im Bereich von
// Millisekunden bis niedrigen Hundert-Millisekunden, keine grosse
// Verzoegerung fuer sich allein. Der spuerbarere Anteil eines
// verlangsamten Erstboots ist storage_manager's LittleFS-Formatierung
// (siehe dort) - siehe docs/entscheidungen.md "Hinweis: erster Boot
// nach dem Flashen dauert laenger".
static bool generate_and_save_host_key(void) {
  WC_RNG rng;
  if (wc_InitRng(&rng) != 0) {
    ESP_LOGE(TAG, "wc_InitRng fehlgeschlagen");
    return false;
  }

  ecc_key key;
  wc_ecc_init(&key);
  int ret = wc_ecc_make_key(&rng, 32, &key);  // 32 Byte = 256 Bit = P-256
  if (ret != 0) {
    ESP_LOGE(TAG, "wc_ecc_make_key fehlgeschlagen: %d", ret);
    wc_ecc_free(&key);
    wc_FreeRng(&rng);
    return false;
  }

  int der_len = wc_EccKeyToDer(&key, s_host_key, sizeof(s_host_key));
  wc_ecc_free(&key);
  wc_FreeRng(&rng);
  if (der_len <= 0) {
    ESP_LOGE(TAG, "wc_EccKeyToDer fehlgeschlagen: %d", der_len);
    return false;
  }
  s_host_key_len = (word32)der_len;

  FILE* f = fopen(HOST_KEY_FILE, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Konnte %s nicht schreiben", HOST_KEY_FILE);
    return false;
  }
  fwrite(s_host_key, 1, s_host_key_len, f);
  fclose(f);
  ESP_LOGI(TAG, "Neuer SSH-Host-Key erzeugt und gespeichert (%lu Byte DER)", (unsigned long)s_host_key_len);
  return true;
}

// =============================================================================
// Oeffentliche Host-Key-Anzeige (Fingerprint + volle Zeile im
// OpenSSH-Format) fuer die Uebersichtsseite - nicht vertraulich, wird
// beim Handshake ohnehin an jeden Client uebertragen. wolfSSH selbst
// bietet keine Export-/Fingerprint-Hilfsfunktion (geprueft in
// wolfssh/ssh.h), deshalb von Hand ueber wolfCrypt aufgebaut:
// wc_EccPrivateKeyDecode dekodiert den persistierten DER-Key zurueck in
// ein ecc_key, wc_ecc_export_x963 liefert den oeffentlichen Punkt im
// Format 0x04||X||Y - das ist bytegleich mit dem "Q"-Feld, das das
// SSH-Wire-Format (RFC 5656) fuer ECDSA-Keys erwartet, also direkt
// verwendbar ohne Umbauen.
// =============================================================================

static size_t write_ssh_string(uint8_t* out, const uint8_t* data, size_t len) {
  out[0] = (uint8_t)((len >> 24) & 0xFF);
  out[1] = (uint8_t)((len >> 16) & 0xFF);
  out[2] = (uint8_t)((len >> 8) & 0xFF);
  out[3] = (uint8_t)(len & 0xFF);
  memcpy(out + 4, data, len);
  return 4 + len;
}

static void base64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap, bool pad) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t oi = 0, i = 0;
  for (; i + 3 <= in_len && oi + 4 < out_cap; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[oi++] = t[(v >> 18) & 0x3F];
    out[oi++] = t[(v >> 12) & 0x3F];
    out[oi++] = t[(v >> 6) & 0x3F];
    out[oi++] = t[v & 0x3F];
  }
  size_t rem = in_len - i;
  if (rem == 1 && oi + 4 < out_cap) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[oi++] = t[(v >> 18) & 0x3F];
    out[oi++] = t[(v >> 12) & 0x3F];
    if (pad) {
      out[oi++] = '=';
      out[oi++] = '=';
    }
  } else if (rem == 2 && oi + 4 < out_cap) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[oi++] = t[(v >> 18) & 0x3F];
    out[oi++] = t[(v >> 12) & 0x3F];
    out[oi++] = t[(v >> 6) & 0x3F];
    if (pad) out[oi++] = '=';
  }
  out[oi] = '\0';
}

static void compute_host_key_display(void) {
  word32 idx = 0;
  ecc_key key;
  wc_ecc_init(&key);
  if (wc_EccPrivateKeyDecode(s_host_key, &idx, &key, s_host_key_len) != 0) {
    ESP_LOGW(TAG, "Host-Key konnte fuer die Anzeige nicht dekodiert werden");
    wc_ecc_free(&key);
    return;
  }

  uint8_t point[65];
  word32 point_len = sizeof(point);
  if (wc_ecc_export_x963(&key, point, &point_len) != 0) {
    ESP_LOGW(TAG, "Oeffentlicher Punkt konnte nicht exportiert werden");
    wc_ecc_free(&key);
    return;
  }
  wc_ecc_free(&key);

  uint8_t blob[128];
  size_t off = 0;
  off += write_ssh_string(blob + off, (const uint8_t*)SSH_KEY_TYPE, strlen(SSH_KEY_TYPE));
  off += write_ssh_string(blob + off, (const uint8_t*)SSH_CURVE_NAME, strlen(SSH_CURVE_NAME));
  off += write_ssh_string(blob + off, point, point_len);

  char b64[200];
  base64_encode(blob, off, b64, sizeof(b64), true);
  snprintf(s_public_key_line, sizeof(s_public_key_line), "%s %s esp-bmc", SSH_KEY_TYPE, b64);

  Sha256 sha;
  uint8_t hash[32];
  wc_InitSha256(&sha);
  wc_Sha256Update(&sha, blob, off);
  wc_Sha256Final(&sha, hash);

  char hash_b64[48];
  // Kein Padding - so zeigt auch ssh-keygen -lf den Fingerprint an.
  base64_encode(hash, sizeof(hash), hash_b64, sizeof(hash_b64), false);
  snprintf(s_fingerprint, sizeof(s_fingerprint), "SHA256:%s", hash_b64);
}

const char* ssh_manager_get_host_key_fingerprint(void) { return s_fingerprint; }
const char* ssh_manager_get_host_public_key_line(void) { return s_public_key_line; }

// =============================================================================
// Benutzeranmeldung - dieselbe Kontodatenbank wie Web/USB (user_manager),
// wie mit dem Nutzer festgelegt: der ESP betreibt einen eigenen SSH-
// Server, kein Pass-Through zu einer sshd auf dem gesteuerten PC.
// Passwort UND Public-Key (webconfig.txt: "... + hinterlegen eines SSH
// key"). Mindestrolle SSH_USER (Rollenliste: "SSH User = web Console +
// ... SSH").
// =============================================================================

typedef struct {
  char username[32];
  char client_ip[16];  // Quell-IP, vor dem Handshake gesetzt - fuer die Login-Protokollierung
  user_role_t role;
  bool authenticated;
} ssh_session_ctx_t;

// Aktuell aktive SSH-Konsolen-Sitzung (fuer die Uebersichtsseite: "wird SSH
// gerade genutzt und von wem"). Gesetzt beim Konsolen-Claim, geraeumt beim
// Release - aber nur, wenn dieselbe Generation noch eingetragen ist, damit ein
// abbauendes altes Session-Task bei einem Takeover nicht die Infos der neuen
// Sitzung loescht. Schlichte globale Felder genuegen (ein einziger Konsumer der
// Konsole zur Zeit, nur kurze Ueberlappung beim Takeover).
static volatile bool s_console_active = false;
static char s_console_user[32] = "";
static char s_console_ip[16] = "";
static uint32_t s_console_gen = 0;

bool ssh_manager_get_active_console(char* user, size_t user_cap, char* ip, size_t ip_cap) {
  if (!s_console_active) return false;
  if (user && user_cap) {
    strncpy(user, s_console_user, user_cap - 1);
    user[user_cap - 1] = '\0';
  }
  if (ip && ip_cap) {
    strncpy(ip, s_console_ip, ip_cap - 1);
    ip[ip_cap - 1] = '\0';
  }
  return true;
}

static const char* role_name(user_role_t role) {
  switch (role) {
    case USER_ROLE_ADMIN: return "Admin";
    case USER_ROLE_VERWALTER: return "Verwalter";
    case USER_ROLE_SSH_USER: return "SSH User";
    default: return "Leser";
  }
}

static int ws_user_auth(byte authType, WS_UserAuthData* authData, void* ctx) {
  ssh_session_ctx_t* sctx = (ssh_session_ctx_t*)ctx;

  char username[32];
  size_t ulen = authData->usernameSz < sizeof(username) - 1 ? authData->usernameSz : sizeof(username) - 1;
  memcpy(username, authData->username, ulen);
  username[ulen] = '\0';

  if (authType == WOLFSSH_USERAUTH_PASSWORD) {
    char password[64];
    size_t plen = authData->sf.password.passwordSz < sizeof(password) - 1 ? authData->sf.password.passwordSz
                                                                           : sizeof(password) - 1;
    memcpy(password, authData->sf.password.password, plen);
    password[plen] = '\0';

    user_role_t role;
    if (!user_manager_authenticate(username, password, &role) || role < USER_ROLE_SSH_USER) {
      ESP_LOGW(TAG, "SSH-Passwort-Login fehlgeschlagen fuer \"%s\"", username);
      return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    }

    strncpy(sctx->username, username, sizeof(sctx->username) - 1);
    sctx->role = role;
    sctx->authenticated = true;

    char event[96];
    snprintf(event, sizeof(event), "Login (SSH, Passwort): %s (%s) von %s", username, role_name(role),
             sctx->client_ip);
    audit_log_add(event);
    ESP_LOGI(TAG, "%s", event);
    return WOLFSSH_USERAUTH_SUCCESS;
  }

  if (authType == WOLFSSH_USERAUTH_PUBLICKEY) {
    user_role_t role;
    if (!user_manager_verify_ssh_public_key(username, authData->sf.publicKey.publicKey,
                                             authData->sf.publicKey.publicKeySz, &role) ||
        role < USER_ROLE_SSH_USER) {
      ESP_LOGW(TAG, "SSH-Public-Key-Login abgelehnt fuer \"%s\"", username);
      return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
    }

    // Zwei Runden im SSH-Pubkey-Auth-Protokoll: erst eine unsignierte
    // Anfrage ("waere dieser Schluessel akzeptiert?"), dann - nur wenn
    // ja - eine signierte. wolfSSH prueft die Signatur selbst, bevor es
    // diesen Callback mit hasSignature=1 aufruft; wir muessen nur
    // pruefen, ob der Schluessel zu diesem Benutzer gehoert (in beiden
    // Runden identisch), und erst bei der signierten Runde final als
    // angemeldet gelten.
    strncpy(sctx->username, username, sizeof(sctx->username) - 1);
    sctx->role = role;
    if (authData->sf.publicKey.hasSignature) {
      sctx->authenticated = true;
      char event[96];
      snprintf(event, sizeof(event), "Login (SSH, Public-Key): %s (%s) von %s", username, role_name(role),
               sctx->client_ip);
      audit_log_add(event);
      ESP_LOGI(TAG, "%s", event);
    }
    return WOLFSSH_USERAUTH_SUCCESS;
  }

  return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

// =============================================================================
// Session-Bruecke: dieselbe CDC-Queue/-write wie die WebSocket-Konsole
// (usb_manager), exklusiv ueber usb_manager_console_claim/_release
// (siehe usb_manager.h - genau ein Konsolen-Konsument gleichzeitig).
// =============================================================================

static void handle_session(int client_fd, const char* client_ip) {
  struct timeval tv = {.tv_sec = 0, .tv_usec = SOCKET_TIMEOUT_MS * 1000};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // TCP-Keepalive: ein hart getrennter Client (gekillter Prozess, Netzabriss)
  // sendet kein EOF/RST. wolfSSH_worker() liefert dann bei jedem recv nur den
  // SO_RCVTIMEO-Timeout (WS_WANT_READ), was die Session-Schleife als "weiter"
  // wertet -> sie spinnt endlos und blockiert, da ssh_task single-threaded
  // handle_session() inline aufruft, den accept()-Loop dauerhaft (Port 22 tot
  // bis zum Reboot). Mit Keepalive erkennt der Kernel den toten Peer nach
  // ~KEEPIDLE + KEEPCNT*KEEPINTVL (hier ~30 s) Ruhe, der Socket meldet einen
  // Fehler -> wolfSSH_worker() gibt WS_SOCKET_ERROR_E -> Schleife bricht ab.
  // Eine legitim untaetige, aber lebende Session bleibt unberuehrt (ihr
  // TCP-Stack beantwortet die Proben automatisch). lwIP-Werte in Sekunden.
  int ka_on = 1, ka_idle = 15, ka_intvl = 5, ka_cnt = 3;
  setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka_on, sizeof(ka_on));
  setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
  setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
  setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof(ka_cnt));

  WOLFSSH* ssh = wolfSSH_new(s_ctx);
  if (!ssh) {
    ESP_LOGE(TAG, "wolfSSH_new fehlgeschlagen");
    close(client_fd);
    return;
  }

  ssh_session_ctx_t sctx = {0};
  strncpy(sctx.client_ip, client_ip, sizeof(sctx.client_ip) - 1);  // vor dem Handshake fuer die Login-Protokollierung
  wolfSSH_SetUserAuthCtx(ssh, &sctx);
  wolfSSH_set_fd(ssh, client_fd);

  int ret;
  int wserr = 0;
  TickType_t hs_start = xTaskGetTickCount();
  do {
    ret = wolfSSH_accept(ssh);
    if (ret == WS_SUCCESS) {
      break;
    }
    // WICHTIG: wolfSSH_accept liefert bei "wuerde blockieren" WS_FATAL_ERROR
    // (-1001) zurueck und signalisiert WS_WANT_READ/WS_WANT_WRITE NUR ueber
    // wolfSSH_get_error() (siehe ssh.c: ssh->error wird beim naechsten Aufruf
    // wieder auf 0 gesetzt). Der Rueckgabewert allein darf daher NICHT als
    // fatal gewertet werden - sonst reisst schon das erste SO_RCVTIMEO-Timeout
    // (200 ms) waehrend der Userauth-Phase (der Server wartet dort auf die
    // Passwort-Eingabe des Nutzers, das dauert laenger als 200 ms) die
    // Verbindung ab, und die Passwort-Pruefung kommt nie zustande.
    wserr = wolfSSH_get_error(ssh);
    if (wserr == WS_WANT_READ || wserr == WS_WANT_WRITE) {
      if (xTaskGetTickCount() - hs_start > pdMS_TO_TICKS(HANDSHAKE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "SSH-Handshake-Timeout von %s - Abbruch", client_ip);
        break;
      }
      continue;  // Handshake noch nicht fertig - erneut versuchen
    }
    break;  // echter Fehler
  } while (1);

  if (ret != WS_SUCCESS || !sctx.authenticated) {
    ESP_LOGW(TAG, "SSH-Handshake/Anmeldung fehlgeschlagen von %s (wolfSSH-Fehler %d)", client_ip, wserr);
    wolfSSH_free(ssh);
    close(client_fd);
    return;
  }

  // Takeover statt Ablehnung: eine neue SSH-Sitzung uebernimmt die (einzige)
  // Konsole IMMER und verdraengt eine bestehende Web-/SSH-Sitzung. Die vorige
  // Sitzung erkennt den Verlust ueber usb_manager_console_is_current() und
  // beendet sich (der accept()-Loop laesst dafuer kurz 2 Sitzungen zu).
  uint32_t console_gen = usb_manager_console_claim(CONSOLE_OWNER_SSH);
  ESP_LOGI(TAG, "SSH-Konsole aktiv (uebernommen) fuer %s (%s) von %s, Generation %u", sctx.username,
           role_name(sctx.role), client_ip, console_gen);

  // Aktive-Sitzung-Anzeige fuer die Uebersichtsseite setzen.
  strncpy(s_console_user, sctx.username, sizeof(s_console_user) - 1);
  s_console_user[sizeof(s_console_user) - 1] = '\0';
  strncpy(s_console_ip, client_ip, sizeof(s_console_ip) - 1);
  s_console_ip[sizeof(s_console_ip) - 1] = '\0';
  s_console_gen = console_gen;
  s_console_active = true;

  QueueHandle_t rx_queue = usb_manager_get_cdc_rx_queue();
  byte iobuf[512];  // groesser, damit der 1-Tick-Yield unten den Ausgabedurchsatz nicht ausbremst
  TickType_t last_activity = xTaskGetTickCount();
  bool prev_cr = false;  // fuer die CR/CRLF->LF-Normalisierung der Client-Eingabe (siehe unten)

  // Shell-Channel ermitteln - er steht nach erfolgreichem Handshake bereits.
  // NICHT ueber "!= 0" pruefen: die serverseitige Channel-ID ist regulaer 0,
  // ein "!= 0"-Wachthund wuerde die gesamte Host->Client-Ausgabe unterdruecken
  // (Mit-Ursache der "leeren Konsole"). Stattdessen ein have_channel-Flag.
  word32 send_channel = 0;
  bool have_channel = false;
  WOLFSSH_CHANNEL* ch0 = wolfSSH_ChannelNext(ssh, NULL);
  if (ch0 && wolfSSH_ChannelGetId(ch0, &send_channel, WS_CHANNEL_ID_SELF) == WS_SUCCESS) {
    have_channel = true;
  }

  // Begruessungs-/Status-Banner beim Verbindungsaufbau (SOL-Konsole zum Host).
  if (have_channel) {
    char banner[512];
    usb_manager_build_status_banner(banner, sizeof(banner));
    wolfSSH_ChannelIdSend(ssh, send_channel, (byte*)banner, (word32)strlen(banner));
  }

  for (;;) {
    if (!usb_manager_console_is_current(console_gen)) break;  // von neuer Sitzung uebernommen

    word32 channelId = 0;
    int wret = wolfSSH_worker(ssh, &channelId);
    if (wret == WS_CHAN_RXD) {
      last_activity = xTaskGetTickCount();  // Lebenszeichen vom Client
      send_channel = channelId;
      have_channel = true;
      int n = wolfSSH_ChannelIdRead(ssh, channelId, iobuf, sizeof(iobuf));
      if (n > 0) {
        // Zeilenenden der Client-Eingabe auf genau ein LF normalisieren, BEVOR
        // sie an den Host gehen: PuTTY & Co. senden bei Enter nur ein CR (0x0D),
        // die Host-Konsole (PowerShell ReadLine) erwartet aber LF (0x0A). Ohne
        // diese Umsetzung wird die getippte Zeile beim Host nie abgeschlossen
        // -> "nur Banner, kein Prompt, keine Reaktion auf Enter". CR, LF und
        // CRLF (auch ueber Lesegrenzen hinweg via prev_cr) werden zu einem LF.
        byte norm[sizeof(iobuf)];  // <= n Bytes: jedes Eingabebyte -> hoechstens ein Ausgabebyte
        size_t m = 0;
        for (int i = 0; i < n; i++) {
          byte b = iobuf[i];
          if (b == '\r') {
            norm[m++] = '\n';
            prev_cr = true;
          } else if (b == '\n') {
            if (!prev_cr) norm[m++] = '\n';  // einzelnes LF durchlassen, LF nach CR schlucken
            prev_cr = false;
          } else {
            norm[m++] = b;
            prev_cr = false;
          }
        }
        if (m > 0) usb_manager_cdc_write(norm, m);  // normalisierte Client-Eingabe -> Host (SOL)
      }
    } else if (wret == WS_EOF || wret == WS_SOCKET_ERROR_E) {
      break;
    }
    // WS_WANT_READ/WS_WANT_WRITE/WS_SUCCESS/WS_REKEYING/sonstiges: weiter.

    // Host->Client: alles vom gesteuerten PC an den SSH-Client durchreichen.
    if (have_channel) {
      size_t n = 0;
      while (n < sizeof(iobuf) && xQueueReceive(rx_queue, &iobuf[n], 0) == pdTRUE) n++;
      if (n > 0) {
        int sret = wolfSSH_ChannelIdSend(ssh, send_channel, iobuf, (word32)n);
        if (sret == WS_EOF || sret == WS_SOCKET_ERROR_E) break;  // Peer weg beim Senden
        last_activity = xTaskGetTickCount();  // aktive Host-Ausgabe zaehlt bei SOL als Lebenszeichen
      }
    }

    // Idle-Timeout: seit SESSION_IDLE_TIMEOUT_MS kein Byte in KEINER Richtung.
    // (Fuer SOL zaehlt bewusst auch die Host->Client-Ausgabe als Lebenszeichen,
    // damit reines "Zuschauen" nicht getrennt wird; ein hart getrennter Peer
    // wird ueber TCP-Keepalive und die WS_SOCKET_ERROR_E-Abbrueche oben erkannt.)
    if (xTaskGetTickCount() - last_activity > pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS)) {
      ESP_LOGW(TAG, "SSH-Sitzung Idle-Timeout (%s) - Trennung", client_ip);
      break;
    }

    // Fairness/Watchdog: wurde in dieser Runde KEINE Client-Eingabe verarbeitet,
    // kurz an IDLE abgeben. Bei fliessender Host-Ausgabe kehrt wolfSSH_worker
    // durch die Fenster-Updates des Clients sofort zurueck - ohne diesen Yield
    // wuerde der Loop (Prio 4) IDLE0 aushungern -> Task-Watchdog-Panik/Reboot
    // (dokumentierte IDLE-Empfindlichkeit dieses Boards). Bei Eingabe kein Delay
    // (voll responsiv); der groessere iobuf haelt den Ausgabedurchsatz hoch.
    if (wret != WS_CHAN_RXD) {
      vTaskDelay(1);
    }
  }

  usb_manager_console_release(console_gen);
  // Aktive-Sitzung-Anzeige nur raeumen, wenn seither KEINE neue Sitzung
  // uebernommen hat (sonst wuerde dieses abbauende alte Task die Infos der
  // neuen Sitzung ueberschreiben) - Vergleich ueber die Konsolen-Generation.
  if (s_console_gen == console_gen) {
    s_console_active = false;
    s_console_user[0] = '\0';
    s_console_ip[0] = '\0';
  }
  ESP_LOGI(TAG, "SSH-Sitzung beendet: %s (%s)", sctx.username, client_ip);
  char event[96];
  snprintf(event, sizeof(event), "SSH-Sitzung beendet: %s von %s", sctx.username, client_ip);
  audit_log_add(event);

  wolfSSH_shutdown(ssh);
  wolfSSH_free(ssh);
  close(client_fd);
}

// Zaehler aktiver Session-Tasks. Nur ssh_task erhoeht (einziger Spawner), die
// jeweilige session_task erniedrigt beim Beenden - kein zusaetzlicher Lock noetig.
static volatile int s_active_sessions = 0;

typedef struct {
  int client_fd;
  char client_ip[16];
} ssh_session_arg_t;

// Laeuft pro Verbindung in einer eigenen Task, damit handle_session() den
// accept()-Loop nie blockieren kann. Gibt Argument + Slot beim Ende frei.
static void session_task(void* arg) {
  ssh_session_arg_t* sa = (ssh_session_arg_t*)arg;
  handle_session(sa->client_fd, sa->client_ip);
  free(sa);
  s_active_sessions--;
  vTaskDelete(NULL);
}

static void ssh_task(void* arg) {
  (void)arg;

  int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "Socket konnte nicht erstellt werden");
    vTaskDelete(NULL);
    return;
  }
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(SSH_PORT);
  if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "Bind auf Port %d fehlgeschlagen", SSH_PORT);
    close(listen_fd);
    vTaskDelete(NULL);
    return;
  }
  // Backlog 2 - passt zu MAX_SSH_SESSIONS=2 (Takeover-Ueberlappung): die neue
  // Verbindung soll angenommen werden koennen, waehrend die alte noch laeuft.
  if (listen(listen_fd, 2) != 0) {
    ESP_LOGE(TAG, "Listen fehlgeschlagen");
    close(listen_fd);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "SshManager gestartet (Port %d)", SSH_PORT);

  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) continue;

    char client_ip[16];
    inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));

    // Nicht-blockierend: ist bereits eine Sitzung aktiv (auch eine haengende,
    // die noch abgebaut wird), Verbindung sofort schliessen statt den accept()-
    // Loop zu blockieren. Sonst eigene Task fuer die Sitzung.
    if (s_active_sessions >= MAX_SSH_SESSIONS) {
      ESP_LOGW(TAG, "SSH-Verbindung von %s abgewiesen - Sitzung belegt", client_ip);
      close(client_fd);
      continue;
    }
    ssh_session_arg_t* sa = malloc(sizeof(*sa));
    if (!sa) {
      close(client_fd);
      continue;
    }
    sa->client_fd = client_fd;
    memcpy(sa->client_ip, client_ip, sizeof(client_ip));

    ESP_LOGI(TAG, "SSH-Verbindung von %s", client_ip);
    s_active_sessions++;
    if (xTaskCreate(session_task, "ssh_session", 12288, sa, 4, NULL) != pdPASS) {
      s_active_sessions--;
      free(sa);
      close(client_fd);
      ESP_LOGE(TAG, "SSH-Session-Task konnte nicht erstellt werden");
    }
  }
}

void ssh_manager_init(void) {
  if (!load_host_key()) {
    if (!generate_and_save_host_key()) {
      ESP_LOGE(TAG, "Kein SSH-Host-Key verfuegbar - SshManager startet nicht");
      return;
    }
  }

  compute_host_key_display();

  s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
  if (!s_ctx) {
    ESP_LOGE(TAG, "wolfSSH_CTX_new fehlgeschlagen");
    return;
  }
  if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, s_host_key, s_host_key_len, WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
    ESP_LOGE(TAG, "Host-Key konnte nicht geladen werden");
    return;
  }
  wolfSSH_SetUserAuth(s_ctx, ws_user_auth);

  // 8 KB statt der anfangs veranschlagten 6 KB: ECC-Handshake (ECDH +
  // Host-Key-Signatur) und die laufende AES/ChaCha20-Verschluesselung
  // laufen alle auf demselben Task-Stack wie handle_session() (keine
  // eigene Task pro Verbindung) - grosszuegig bemessen, siehe die
  // sm-Stack-Overflow-Lehre aus dem Sensormeter-Projekt (Guru Meditation
  // durch einen zu knapp bemessenen Task-Stack).
  xTaskCreate(ssh_task, "ssh_manager", 8192, NULL, 4, NULL);
}
