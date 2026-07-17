#include "wireguard_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wireguard.h"

static const char* TAG = "wireguard_manager";
#define WG_CONFIG_FILE "/storage/wireguard.json"
#define MAX_EXTRA_ALLOWED 4

static wireguard_ctx_t s_ctx = {0};
static bool s_has_uploaded_config = false;

// Persistente Kopien (esp_wireguard haelt nur Zeiger, die Strings muessen
// die ganze Laufzeit ueber gueltig bleiben) - bei Kconfig-Platzhalterwerten
// sind das ohnehin statische Compile-Time-Strings, bei einem Upload werden
// diese Puffer stattdessen gefuellt.
static char s_private_key[64];
static char s_public_key[64];
static char s_local_address[16];
static char s_local_netmask[16];
static char s_endpoint_host[64];
static int s_listen_port;
static int s_endpoint_port;

typedef struct {
  char ip[16];
  char mask[16];
} allowed_ip_t;
static allowed_ip_t s_extra_allowed[MAX_EXTRA_ALLOWED];
static int s_extra_allowed_count;

static void trim(char* s) {
  char* start = s;
  while (*start == ' ' || *start == '\t' || *start == '\r') start++;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) s[--len] = '\0';
}

static void cidr_prefix_to_netmask(int prefix, char* out, size_t out_len) {
  uint32_t mask = 0;
  if (prefix >= 32) {
    mask = 0xFFFFFFFFu;
  } else if (prefix > 0) {
    mask = ~((1u << (32 - prefix)) - 1);
  }
  snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)((mask >> 24) & 0xFF), (unsigned)((mask >> 16) & 0xFF),
           (unsigned)((mask >> 8) & 0xFF), (unsigned)(mask & 0xFF));
}

// Trennt "ip/prefix" - schreibt die IP nach out_ip, die Prefixlaenge (oder
// 32 falls keine angegeben) als Netzmaske nach out_mask.
static void split_cidr(const char* cidr, char* out_ip, size_t ip_len, char* out_mask, size_t mask_len) {
  char buf[40];
  strncpy(buf, cidr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* slash = strchr(buf, '/');
  int prefix = 32;
  if (slash) {
    *slash = '\0';
    prefix = atoi(slash + 1);
  }
  strncpy(out_ip, buf, ip_len - 1);
  out_ip[ip_len - 1] = '\0';
  cidr_prefix_to_netmask(prefix, out_mask, mask_len);
}

static bool is_default_route(const char* cidr) {
  return strncmp(cidr, "0.0.0.0/0", 9) == 0 || strncmp(cidr, "::/0", 4) == 0;
}

// Parst eine wireguard.conf ([Interface]/[Peer]-INI) direkt in die
// statischen Puffer/den Extra-AllowedIPs-Puffer oben. Entfernt dabei jede
// Default-Route (siehe is_default_route()) - "vor dem Speichern der conf
// default route aus dieser entfernen" (webconfig.txt).
static bool parse_conf(const char* text) {
  char section[16] = "";
  s_extra_allowed_count = 0;
  s_private_key[0] = s_public_key[0] = s_local_address[0] = s_local_netmask[0] = s_endpoint_host[0] = '\0';
  s_listen_port = 51820;
  s_endpoint_port = 51820;

  char line[256];
  size_t li = 0;
  const char* p = text;
  bool done = false;
  while (!done) {
    char c = *p++;
    bool eol = (c == '\n' || c == '\0');
    if (c == '\0') done = true;
    if (!eol && li < sizeof(line) - 1) {
      line[li++] = c;
      continue;
    }
    if (!eol) continue;  // Zeile abgeschnitten, Rest verwerfen

    line[li] = '\0';
    li = 0;
    char* s = line;
    trim(s);

    if (s[0] == '[') {
      char* end = strchr(s, ']');
      if (end) {
        *end = '\0';
        strncpy(section, s + 1, sizeof(section) - 1);
      }
      continue;
    }
    if (s[0] == '\0' || s[0] == '#' || s[0] == ';') continue;

    char* eq = strchr(s, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = s;
    char* value = eq + 1;
    trim(key);
    trim(value);

    if (strcasecmp(section, "Interface") == 0) {
      if (strcasecmp(key, "PrivateKey") == 0) {
        strncpy(s_private_key, value, sizeof(s_private_key) - 1);
      } else if (strcasecmp(key, "Address") == 0) {
        char mask[16];
        split_cidr(value, s_local_address, sizeof(s_local_address), mask, sizeof(mask));
        strncpy(s_local_netmask, mask, sizeof(s_local_netmask) - 1);
      } else if (strcasecmp(key, "ListenPort") == 0) {
        s_listen_port = atoi(value);
      }
    } else if (strcasecmp(section, "Peer") == 0) {
      if (strcasecmp(key, "PublicKey") == 0) {
        strncpy(s_public_key, value, sizeof(s_public_key) - 1);
      } else if (strcasecmp(key, "Endpoint") == 0) {
        char* colon = strrchr(value, ':');
        if (colon) {
          *colon = '\0';
          s_endpoint_port = atoi(colon + 1);
        }
        strncpy(s_endpoint_host, value, sizeof(s_endpoint_host) - 1);
      } else if (strcasecmp(key, "AllowedIPs") == 0) {
        char* tok = strtok(value, ",");
        while (tok) {
          trim(tok);
          if (tok[0] && !is_default_route(tok) && s_extra_allowed_count < MAX_EXTRA_ALLOWED) {
            split_cidr(tok, s_extra_allowed[s_extra_allowed_count].ip, sizeof(s_extra_allowed[0].ip),
                       s_extra_allowed[s_extra_allowed_count].mask, sizeof(s_extra_allowed[0].mask));
            s_extra_allowed_count++;
          } else if (tok[0] && is_default_route(tok)) {
            ESP_LOGI(TAG, "Default-Route \"%s\" aus hochgeladener Konfiguration entfernt", tok);
          }
          tok = strtok(NULL, ",");
        }
      }
    }
  }

  return s_private_key[0] && s_public_key[0] && s_local_address[0] && s_endpoint_host[0];
}

static void save_config_to_storage(void) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "private_key", s_private_key);
  cJSON_AddStringToObject(root, "public_key", s_public_key);
  cJSON_AddStringToObject(root, "address", s_local_address);
  cJSON_AddStringToObject(root, "netmask", s_local_netmask);
  cJSON_AddStringToObject(root, "endpoint", s_endpoint_host);
  cJSON_AddNumberToObject(root, "listen_port", s_listen_port);
  cJSON_AddNumberToObject(root, "endpoint_port", s_endpoint_port);

  cJSON* allowed = cJSON_CreateArray();
  for (int i = 0; i < s_extra_allowed_count; i++) {
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ip", s_extra_allowed[i].ip);
    cJSON_AddStringToObject(entry, "mask", s_extra_allowed[i].mask);
    cJSON_AddItemToArray(allowed, entry);
  }
  cJSON_AddItemToObject(root, "allowed_ips", allowed);

  char* text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) return;

  FILE* f = fopen(WG_CONFIG_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  } else {
    ESP_LOGE(TAG, "Konnte %s nicht schreiben", WG_CONFIG_FILE);
  }
  cJSON_free(text);
}

// Liefert true, wenn eine gespeicherte Konfiguration geladen werden konnte.
static bool load_config_from_storage(void) {
  FILE* f = fopen(WG_CONFIG_FILE, "r");
  if (!f) return false;

  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* root = cJSON_Parse(buf);
  if (!root) return false;

  cJSON* item;
  if ((item = cJSON_GetObjectItem(root, "private_key")) && cJSON_IsString(item)) {
    strncpy(s_private_key, item->valuestring, sizeof(s_private_key) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "public_key")) && cJSON_IsString(item)) {
    strncpy(s_public_key, item->valuestring, sizeof(s_public_key) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "address")) && cJSON_IsString(item)) {
    strncpy(s_local_address, item->valuestring, sizeof(s_local_address) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "netmask")) && cJSON_IsString(item)) {
    strncpy(s_local_netmask, item->valuestring, sizeof(s_local_netmask) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "endpoint")) && cJSON_IsString(item)) {
    strncpy(s_endpoint_host, item->valuestring, sizeof(s_endpoint_host) - 1);
  }
  if ((item = cJSON_GetObjectItem(root, "listen_port")) && cJSON_IsNumber(item)) {
    s_listen_port = item->valueint;
  }
  if ((item = cJSON_GetObjectItem(root, "endpoint_port")) && cJSON_IsNumber(item)) {
    s_endpoint_port = item->valueint;
  }

  s_extra_allowed_count = 0;
  cJSON* allowed = cJSON_GetObjectItem(root, "allowed_ips");
  if (cJSON_IsArray(allowed)) {
    int count = cJSON_GetArraySize(allowed);
    for (int i = 0; i < count && s_extra_allowed_count < MAX_EXTRA_ALLOWED; i++) {
      cJSON* entry = cJSON_GetArrayItem(allowed, i);
      cJSON* ip = cJSON_GetObjectItem(entry, "ip");
      cJSON* mask = cJSON_GetObjectItem(entry, "mask");
      if (cJSON_IsString(ip) && cJSON_IsString(mask)) {
        strncpy(s_extra_allowed[s_extra_allowed_count].ip, ip->valuestring, sizeof(s_extra_allowed[0].ip) - 1);
        strncpy(s_extra_allowed[s_extra_allowed_count].mask, mask->valuestring, sizeof(s_extra_allowed[0].mask) - 1);
        s_extra_allowed_count++;
      }
    }
  }

  cJSON_Delete(root);
  return true;
}

static esp_err_t build_and_init(void) {
  wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
  wg_config.private_key = s_private_key;
  wg_config.listen_port = s_listen_port;
  wg_config.public_key = s_public_key;
  wg_config.address = s_local_address;
  wg_config.netmask = s_local_netmask;
  wg_config.endpoint = s_endpoint_host;
  wg_config.port = s_endpoint_port;

  memset(&s_ctx, 0, sizeof(s_ctx));
  esp_err_t err = esp_wireguard_init(&wg_config, &s_ctx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wireguard_init fehlgeschlagen: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t wireguard_manager_init(void) {
  if (load_config_from_storage()) {
    s_has_uploaded_config = true;
    ESP_LOGI(TAG, "WireGuard-Konfiguration von %s geladen", WG_CONFIG_FILE);
  } else {
    // Kein Upload vorhanden - Kconfig-Platzhalterkonfiguration aus dem
    // P1-Machbarkeits-Spike (siehe docs/entscheidungen.md).
    strncpy(s_private_key, CONFIG_ESP_BMC_WG_PRIVATE_KEY, sizeof(s_private_key) - 1);
    strncpy(s_public_key, CONFIG_ESP_BMC_WG_PEER_PUBLIC_KEY, sizeof(s_public_key) - 1);
    strncpy(s_local_address, CONFIG_ESP_BMC_WG_LOCAL_IP, sizeof(s_local_address) - 1);
    strncpy(s_local_netmask, CONFIG_ESP_BMC_WG_LOCAL_IP_NETMASK, sizeof(s_local_netmask) - 1);
    strncpy(s_endpoint_host, CONFIG_ESP_BMC_WG_PEER_ENDPOINT, sizeof(s_endpoint_host) - 1);
    s_listen_port = CONFIG_ESP_BMC_WG_LOCAL_PORT;
    s_endpoint_port = CONFIG_ESP_BMC_WG_PEER_PORT;
    s_extra_allowed_count = 0;
  }
  return build_and_init();
}

esp_err_t wireguard_manager_connect(void) {
  ESP_LOGI(TAG, "Baue WireGuard-Tunnel auf (%s)", s_has_uploaded_config ? "hochgeladene Konfiguration"
                                                                        : "Platzhalter-Konfiguration, siehe Kconfig");
  esp_err_t err = esp_wireguard_connect(&s_ctx);
  if (err != ESP_OK) return err;

  for (int i = 0; i < s_extra_allowed_count; i++) {
    esp_err_t aerr = esp_wireguard_add_allowed_ip(&s_ctx, s_extra_allowed[i].ip, s_extra_allowed[i].mask);
    if (aerr != ESP_OK) {
      ESP_LOGW(TAG, "Zusaetzliche AllowedIP %s/%s konnte nicht hinzugefuegt werden: %s", s_extra_allowed[i].ip,
                s_extra_allowed[i].mask, esp_err_to_name(aerr));
    }
  }
  return ESP_OK;
}

bool wireguard_manager_is_up(void) { return esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK; }

esp_err_t wireguard_manager_disconnect(void) { return esp_wireguard_disconnect(&s_ctx); }

esp_err_t wireguard_manager_apply_uploaded_config(const char* conf_text) {
  if (!parse_conf(conf_text)) {
    ESP_LOGE(TAG, "Hochgeladene wireguard.conf unvollstaendig (PrivateKey/PublicKey/Address/Endpoint noetig)");
    return ESP_ERR_INVALID_ARG;
  }

  wireguard_manager_disconnect();
  save_config_to_storage();
  s_has_uploaded_config = true;

  esp_err_t err = build_and_init();
  if (err != ESP_OK) return err;
  return wireguard_manager_connect();
}

esp_err_t wireguard_manager_delete_config(void) {
  wireguard_manager_disconnect();
  remove(WG_CONFIG_FILE);
  s_has_uploaded_config = false;
  s_extra_allowed_count = 0;
  ESP_LOGI(TAG, "Gespeicherte WireGuard-Konfiguration geloescht");
  return ESP_OK;
}

bool wireguard_manager_has_uploaded_config(void) { return s_has_uploaded_config; }

void wireguard_manager_get_local_address(char* out, size_t out_len) {
  strncpy(out, s_local_address, out_len - 1);
  out[out_len - 1] = '\0';
}

void wireguard_manager_get_endpoint(char* out, size_t out_len) {
  snprintf(out, out_len, "%s:%d", s_endpoint_host, s_endpoint_port);
}