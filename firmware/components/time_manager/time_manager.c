#include "time_manager.h"

#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "time_manager";

#define NTP_SERVER "de.pool.ntp.org"
#define RESYNC_INTERVAL_MS (5UL * 60UL * 60UL * 1000UL)  // 5 Stunden
#define SYNC_WAIT_MS (15UL * 1000UL)  // Zeitfenster fuer "hat dieser Versuch geklappt?"
// Europe/Berlin inkl. automatischer Sommerzeitumschaltung (POSIX-TZ-String) -
// gleicher Wert wie TZ_GERMANY in der Sensormeter-Familie.
#define TZ_GERMANY "CET-1CEST,M3.5.0,M10.5.0/3"

static SemaphoreHandle_t s_link_up_sem;
static volatile bool s_synced;
static time_manager_sync_result_cb_t s_result_cb;

static void on_time_sync(struct timeval* tv) {
  (void)tv;
  s_synced = true;

  time_t now = time(NULL);
  char buf[32];
  ctime_r(&now, buf);
  buf[strcspn(buf, "\n")] = '\0';  // ctime_r haengt selbst ein '\n' an
  ESP_LOGI(TAG, "NTP synchronisiert: %s", buf);
}

static void time_manager_task(void* arg) {
  (void)arg;
  for (;;) {
    // Wartet auf ein Link-Up-Signal (network_manager) ODER laesst den
    // 5h-Rhythmus ablaufen - "sofort nach Link-Up, sonst alle 5 Stunden".
    if (xSemaphoreTake(s_link_up_sem, pdMS_TO_TICKS(RESYNC_INTERVAL_MS)) == pdTRUE) {
      ESP_LOGI(TAG, "Link-Up erkannt -> NTP-Resync vorgezogen");
    }
    esp_netif_sntp_start();

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SYNC_WAIT_MS));
    bool ok = (err == ESP_OK);
    if (!ok) {
      ESP_LOGW(TAG, "NTP-Sync fehlgeschlagen (%s)", esp_err_to_name(err));
    }
    if (s_result_cb) {
      s_result_cb(ok);
    }
  }
}

void time_manager_init(void) {
  setenv("TZ", TZ_GERMANY, 1);
  tzset();

  s_link_up_sem = xSemaphoreCreateBinary();

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
  config.start = false;  // Start steuert time_manager_task selbst
  config.sync_cb = on_time_sync;
  esp_netif_sntp_init(&config);

  xTaskCreate(time_manager_task, "time_manager", 3072, NULL, 1, NULL);
  ESP_LOGI(TAG, "TimeManager gestartet (Server: %s, Resync alle 5h oder bei Link-Up)", NTP_SERVER);
}

void time_manager_notify_link_up(void) { xSemaphoreGive(s_link_up_sem); }

bool time_manager_is_synced(void) { return s_synced; }

void time_manager_set_sync_result_cb(time_manager_sync_result_cb_t cb) { s_result_cb = cb; }
