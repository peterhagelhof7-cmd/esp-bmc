#include "ssh_manager.h"

#include <stdio.h>
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
  user_role_t role;
  bool authenticated;
} ssh_session_ctx_t;

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

    char event[64];
    snprintf(event, sizeof(event), "Login (SSH, Passwort): %s (%s)", username, role_name(role));
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
      char event[64];
      snprintf(event, sizeof(event), "Login (SSH, Public-Key): %s (%s)", username, role_name(role));
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

  WOLFSSH* ssh = wolfSSH_new(s_ctx);
  if (!ssh) {
    ESP_LOGE(TAG, "wolfSSH_new fehlgeschlagen");
    close(client_fd);
    return;
  }

  ssh_session_ctx_t sctx = {0};
  wolfSSH_SetUserAuthCtx(ssh, &sctx);
  wolfSSH_set_fd(ssh, client_fd);

  int ret;
  do {
    ret = wolfSSH_accept(ssh);
  } while (ret != WS_SUCCESS && (ret == WS_WANT_READ || ret == WS_WANT_WRITE));

  if (ret != WS_SUCCESS || !sctx.authenticated) {
    ESP_LOGW(TAG, "SSH-Handshake/Anmeldung fehlgeschlagen von %s (Fehler %d)", client_ip, ret);
    wolfSSH_free(ssh);
    close(client_fd);
    return;
  }

  if (usb_manager_console_owner() != CONSOLE_OWNER_NONE) {
    ESP_LOGW(TAG, "SSH-Konsole von %s abgelehnt - Konsole bereits belegt (Web oder andere SSH-Sitzung)",
             client_ip);
    wolfSSH_free(ssh);
    close(client_fd);
    return;
  }
  usb_manager_console_claim(CONSOLE_OWNER_SSH);
  ESP_LOGI(TAG, "SSH-Konsole aktiv fuer %s (%s) von %s", sctx.username, role_name(sctx.role), client_ip);

  QueueHandle_t rx_queue = usb_manager_get_cdc_rx_queue();
  word32 activeChannel = 0;
  byte iobuf[128];

  for (;;) {
    if (usb_manager_console_owner() != CONSOLE_OWNER_SSH) break;  // von aussen entzogen

    word32 channelId = 0;
    int wret = wolfSSH_worker(ssh, &channelId);
    if (wret == WS_CHAN_RXD) {
      activeChannel = channelId;
      int n = wolfSSH_ChannelIdRead(ssh, channelId, iobuf, sizeof(iobuf));
      if (n > 0) usb_manager_cdc_write(iobuf, (size_t)n);
    } else if (wret == WS_EOF || wret == WS_SOCKET_ERROR_E) {
      break;
    }
    // WS_WANT_READ/WS_WANT_WRITE/WS_SUCCESS/WS_REKEYING/sonstiges: einfach
    // weiter, kein fataler Zustand.

    if (activeChannel != 0 || channelId != 0) {
      word32 sendChannel = activeChannel != 0 ? activeChannel : channelId;
      size_t n = 0;
      while (n < sizeof(iobuf) && xQueueReceive(rx_queue, &iobuf[n], 0) == pdTRUE) n++;
      if (n > 0) wolfSSH_ChannelIdSend(ssh, sendChannel, iobuf, (word32)n);
    }
  }

  usb_manager_console_release(CONSOLE_OWNER_SSH);
  ESP_LOGI(TAG, "SSH-Sitzung beendet: %s (%s)", sctx.username, client_ip);
  char event[64];
  snprintf(event, sizeof(event), "SSH-Sitzung beendet: %s", sctx.username);
  audit_log_add(event);

  wolfSSH_shutdown(ssh);
  wolfSSH_free(ssh);
  close(client_fd);
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
  // Backlog 1 - nur eine gleichzeitige Sitzung vorgesehen (siehe
  // usb_manager_console_claim/_release, genau ein Konsolen-Konsument).
  if (listen(listen_fd, 1) != 0) {
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
    ESP_LOGI(TAG, "SSH-Verbindung von %s", client_ip);
    handle_session(client_fd, client_ip);
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
