#include "audit_log.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_manager.h"
#include "network_manager.h"
#include "sensor_history.h"
#include "sensor_manager.h"
#include "storage_manager.h"
#include "time_manager.h"
#include "usb_manager.h"
#include "user_manager.h"
#include "web_server_manager.h"
#include "wireguard_manager.h"

static const char* TAG = "esp-bmc";

static void on_ntp_sync_result(bool success) {
  audit_log_add(success ? "NTP-Sync: OK" : "NTP-Sync: fehlgeschlagen");
}

void app_main(void) {
  ESP_LOGI(TAG, "=== ESP-BMC (P0+P2+P1-Spike+P3+Storage+P4+P5) ===");

  storage_manager_init();
  audit_log_init();
  user_manager_init();
  sensor_history_init();
  gpio_manager_init();
  sensor_manager_init();
  usb_manager_init();
  // Vor network_manager_init() - dessen Event-Handler benachrichtigt
  // time_manager bei jedem Link-Up, der Semaphore muss dafuer schon
  // existieren.
  time_manager_init();
  time_manager_set_sync_result_cb(on_ntp_sync_result);
  network_manager_init();

  // wireguard_manager_init() ist immer sicher (baut nur den lokalen Kontext
  // auf, sendet nichts) - laedt eine per Web-UI hochgeladene Konfiguration
  // von der storage-Partition, falls vorhanden, sonst die
  // Kconfig-Platzhalterkonfiguration aus dem P1-Spike.
  ESP_ERROR_CHECK(wireguard_manager_init());

  // Tatsaechlich verbinden nur, wenn entweder eine echte (hochgeladene)
  // Konfiguration vorliegt, oder der Entwickler-Platzhaltertest ueber
  // Kconfig explizit gewuenscht ist (P1-Machbarkeits-Spike mit
  // Platzhalter-Schluesseln, nur fuer Kompilier-/Footprint-Zwecke).
  // CONFIG_ESP_BMC_WG_ENABLE existiert als C-Bezeichner nur, wenn der
  // Kconfig-Bool auf "y" steht - daher der #if-Umweg statt einer direkten
  // Laufzeitabfrage.
  bool wg_dev_test_requested = false;
#if CONFIG_ESP_BMC_WG_ENABLE
  wg_dev_test_requested = true;
#endif
  if (wireguard_manager_has_uploaded_config() || wg_dev_test_requested) {
    while (!network_manager_is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    wireguard_manager_connect();
  }

  web_server_manager_init();

  bool tastschutz_log_state = false;
  for (;;) {
    bool tastschutz = config_manager_is_tastschutz_active();
    if (tastschutz != tastschutz_log_state) {
      ESP_LOGI(TAG, "Tastschutz: %s", tastschutz ? "AKTIV" : "inaktiv");
      tastschutz_log_state = tastschutz;
    }

    ESP_LOGI(TAG, "Power: erfasst=%d weitergeleitet=%d | Reset: erfasst=%d weitergeleitet=%d",
             gpio_manager_power_taste_gedrueckt(), gpio_manager_power_taste_weitergeleitet(),
             gpio_manager_reset_taste_gedrueckt(), gpio_manager_reset_taste_weitergeleitet());

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
