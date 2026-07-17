#include "network_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "time_manager.h"

static const char* TAG = "network_manager";
#define WLAN_CONFIG_FILE "/storage/wlan.json"

static volatile bool s_connected = false;
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

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  (void)arg;
  (void)event_data;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    ESP_LOGW(TAG, "WLAN getrennt, Reconnect-Versuch");
    esp_wifi_connect();
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

  wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

  if (load_wlan_from_storage()) {
    ESP_LOGI(TAG, "WLAN-Zugangsdaten von %s geladen", WLAN_CONFIG_FILE);
  } else {
    // Kein Upload vorhanden - Kconfig-Platzhalterkonfiguration aus dem
    // P0-Mindestumfang (siehe docs/entscheidungen.md).
    strncpy(s_ssid, CONFIG_ESP_BMC_WIFI_SSID, sizeof(s_ssid) - 1);
    strncpy(s_password, CONFIG_ESP_BMC_WIFI_PASSWORD, sizeof(s_password) - 1);
  }

  wifi_config_t wifi_config = {0};
  strncpy((char*)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char*)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = s_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

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

  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  s_connected = false;
  esp_wifi_disconnect();  // Fehler ignoriert, falls noch nie verbunden
  esp_wifi_connect();
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
  wifi_scan_config_t scan_config = {0};
  if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return 0;  // blockierend

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count > (uint16_t)max_results) ap_count = (uint16_t)max_results;
  if (ap_count == 0) return 0;

  wifi_ap_record_t* records = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!records) return 0;

  esp_err_t err = esp_wifi_scan_get_ap_records(&ap_count, records);
  if (err != ESP_OK) {
    free(records);
    return 0;
  }

  for (int i = 0; i < ap_count; i++) {
    strncpy(out[i].ssid, (const char*)records[i].ssid, sizeof(out[i].ssid) - 1);
    out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
    out[i].open = (records[i].authmode == WIFI_AUTH_OPEN);
    out[i].rssi = records[i].rssi;
  }
  free(records);
  return ap_count;
}
