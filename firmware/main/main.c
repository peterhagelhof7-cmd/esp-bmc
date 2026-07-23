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

  // WireGuard-Boot-Absturz (2026-07-23) an der Wurzel behoben: der Crash
  // steckte in esp_wireguard/wireguard-platform.c's psa_crypto_init()
  // (mbedTLS-4.x/tf-psa-crypto <-> esp_wireguard-Fork, LoadProhibited).
  // WireGuards eigentliche Krypto (BLAKE2s/ChaCha20Poly1305 via crypto/refc/,
  // X25519 via libsodium) nutzt PSA gar nicht - PSA diente dort nur der
  // Zufallsbyte-Erzeugung. Deshalb wird der PSA-Pfad per idempotentem
  // CMake-Patch (firmware/CMakeLists.txt) durch den Hardware-RNG
  // esp_fill_random() ersetzt und psa_crypto_init() faellt weg - derselbe
  // Weg, den die ESP8266-/LibreTiny-Zweige derselben Datei schon gehen.
  // Siehe docs/entscheidungen.md.
  //
  // wireguard_manager_init() wird trotzdem nur aufgerufen, wenn WireGuard
  // gebraucht wird (hochgeladene Konfiguration vorhanden, geprueft OHNE
  // init() vorher - wireguard_manager_config_available()) oder der
  // Kconfig-Entwicklertest gewuenscht ist - kein Absturzschutz mehr,
  // sondern schlicht sinnvoll: ohne Konfiguration gibt es nichts zu tun.
  // CONFIG_ESP_BMC_WG_ENABLE existiert als C-Bezeichner nur, wenn der
  // Kconfig-Bool auf "y" steht - daher der #if-Umweg.
  bool wg_dev_test_requested = false;
#if CONFIG_ESP_BMC_WG_ENABLE
  wg_dev_test_requested = true;
#endif
  if (wireguard_manager_config_available() || wg_dev_test_requested) {
    ESP_ERROR_CHECK(wireguard_manager_init());
    while (!network_manager_is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    // esp_wireguard_connect() loest den Endpoint-Hostnamen asynchron auf und
    // gibt ESP_ERR_RETRY zurueck, solange die DNS-Aufloesung noch laeuft -
    // der DNS-Callback stoesst den Handshake NICHT selbst an, das passiert
    // erst beim naechsten connect()-Aufruf nach fertigem DNS. Deshalb hier
    // eine begrenzte Retry-Schleife (Blockieren im app_main-Task ist
    // unkritisch; im Web-Upload-Handler waere es das nicht, daher nur hier).
    // Der Netif wird beim ersten Versuch erzeugt, Folgeversuche ueberspringen
    // das und warten nur auf die (dann gecachte) DNS-Antwort.
    for (int attempt = 1; attempt <= 15; attempt++) {
      esp_err_t werr = wireguard_manager_connect();
      if (werr == ESP_OK) {
        ESP_LOGI(TAG, "WireGuard-Tunnel aufgebaut (Versuch %d)", attempt);
        break;
      }
      ESP_LOGW(TAG, "WireGuard-Connect Versuch %d: %s - erneuter Versuch in 2s",
               attempt, esp_err_to_name(werr));
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  } else {
    ESP_LOGI(TAG, "WireGuard uebersprungen - keine hochgeladene Konfiguration vorhanden");
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
