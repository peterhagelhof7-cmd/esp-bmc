#include "audit_log.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "firmware_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_manager.h"
#include "network_manager.h"
#include "notification_manager.h"
#include "ota_manager.h"
#include "sensor_history.h"
#include "sensor_manager.h"
#include "snmp_manager.h"
#include "ssh_manager.h"
#include "storage_manager.h"
#include "time_manager.h"
#include "usb_manager.h"
#include "user_manager.h"
#include "watchdog_manager.h"
#include "web_server_manager.h"
#include "wireguard_manager.h"

static const char* TAG = "esp-bmc";

static void on_ntp_sync_result(bool success) {
  audit_log_add(success ? "NTP-Sync: OK" : "NTP-Sync: fehlgeschlagen");
}

void app_main(void) {
  ESP_LOGI(TAG, "=== ESP-BMC %s (P0+P2+P1-Spike+P3+Storage+P4+P5) ===", DEVICE_FIRMWARE_VERSION);

  // Bestaetigt der Bootloader-Rollback-Ueberwachung (CONFIG_BOOTLOADER_
  // APP_ROLLBACK_ENABLE, sdkconfig.defaults), dass diese Firmware
  // erfolgreich gestartet ist - ohne diesen Aufruf wuerde ein zweiter
  // Boot-Versuch (z.B. nach einem Absturz kurz nach dem Start) automatisch
  // auf die vorherige funktionierende Partition zurueckrollen. Bewusst
  // ganz am Anfang aufgerufen (nicht erst nach einer aufwendigen
  // Selbsttest-Sequenz) - fuer dieses Projekt reicht "startet ueberhaupt
  // bis hierher" als Gesundheitskriterium, kein eigener Health-Check
  // gebaut. Ueber die reine Sensormeter-Parity hinaus (dort nicht
  // vorhanden) - ESP-IDF bietet das nahezu kostenlos mit, siehe
  // docs/entscheidungen.md "OTA-Update ...".
  esp_ota_mark_app_valid_cancel_rollback();

  // Ganz am Anfang, noch vor storage_manager_init() - die RGB-LED soll
  // schon waehrend der (nur beim allerersten Boot spuerbaren)
  // LittleFS-Formatierung sichtbar Farben wechseln, statt den Eindruck
  // eines eingefrorenen Geraets zu erwecken. Siehe docs/entscheidungen.md
  // "Watchdog-LED (RGB, GPIO48)" und "Hinweis: erster Boot nach dem
  // Flashen dauert laenger".
  watchdog_manager_init();

  storage_manager_init();
  config_manager_init();
  audit_log_init();
  user_manager_init();
  // Nach user_manager_init() - notification_manager_trigger() liest die
  // Empfaengerliste aus user_manager (E-Mail + Aktiv-Schalter je Konto).
  notification_manager_init();
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

  ota_manager_init();
  web_server_manager_init();
  snmp_manager_init();
  ssh_manager_init();

  // main-Task ebenfalls beim TWDT anmelden - 1s-Zyklus ist komfortabel
  // unter CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s), siehe watchdog_manager.
  esp_task_wdt_add(NULL);

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

    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
