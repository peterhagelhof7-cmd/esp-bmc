#include "watchdog_manager.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char* TAG = "watchdog_manager";

#define RGB_LED_GPIO 48  // "board mit bezeichner.bmp": IO48_RGB: WS2812
#define COLOR_STEP_MS 40
#define HUE_STEP_DEG 2  // 360/2 * 40ms = 7,2s pro voller Farbumlauf

// Niedrig, aber ueber Idle (0) - diese Task soll auch dann noch
// bevorzugt laufen, wenn andere Anwendungs-Tasks (Prioritaet 3-5, siehe
// z.B. ssh_manager) beschaeftigt sind, damit ein Einfrieren dieser Task
// tatsaechlich auf ein Scheduler-Problem hindeutet und nicht nur auf
// gewoehnliche Prioritaets-Verdraengung.
#define WATCHDOG_TASK_PRIORITY 2
#define WATCHDOG_TASK_STACK 3072

static led_strip_handle_t s_strip;

// Reine Lebenszeichen-Anzeige fuer die ESP-eigene FreeRTOS-Firmware
// (NICHT fuer das Host-Betriebssystem des gesteuerten PCs - das war ein
// frueherer, vom Nutzer explizit verworfener Ansatz). Zwei
// Wirkungsebenen, siehe docs/entscheidungen.md "Watchdog-LED (RGB,
// GPIO48)":
//  1. Allein das regelmaessige Fortschreiten des Farbverlaufs beweist,
//     dass der FreeRTOS-Scheduler diese (niedrig priorisierte) Task
//     ueberhaupt noch bedient - eine andere Task, die die CPU in einer
//     Endlosschleife ohne Yield blockiert, wuerde den Farbverlauf
//     sichtbar einfrieren lassen.
//  2. Diese Task ist zusaetzlich beim ESP-IDF-eigenen Task Watchdog
//     Timer (TWDT) angemeldet (esp_task_wdt_add) und fuettert ihn jeden
//     Zyklus (esp_task_wdt_reset) - bleibt sie aus irgendeinem Grund
//     trotzdem haengen (z.B. durch einen Fehler im led_strip-Treiber
//     selbst), loest das nach CONFIG_ESP_TASK_WDT_TIMEOUT_S einen
//     Panic+Reboot aus (CONFIG_ESP_TASK_WDT_PANIC=y, siehe
//     sdkconfig.defaults) - echte Selbstheilung, nicht nur Anzeige.
static void watchdog_led_task(void* arg) {
  (void)arg;
  esp_task_wdt_add(NULL);

  uint16_t hue = 0;
  for (;;) {
    // value=40 statt 255: als Dauer-Statusanzeige gedacht, nicht als
    // helle Beleuchtung - 40 ist gut erkennbar, aber nicht blendend.
    led_strip_set_pixel_hsv(s_strip, 0, hue, 255, 40);
    led_strip_refresh(s_strip);
    hue = (hue + HUE_STEP_DEG) % 360;

    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(COLOR_STEP_MS));
  }
}

void watchdog_manager_init(void) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = RGB_LED_GPIO,
      .max_leds = 1,
      .led_pixel_format = LED_PIXEL_FORMAT_GRB,
      .led_model = LED_MODEL_WS2812,
      .flags = {.invert_out = 0},
  };
  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,  // 10 MHz, vom Treiber empfohlener Standardwert
      .mem_block_symbols = 0,             // 0 = Treiber-Standardgroesse
      .flags = {.with_dma = 0},           // eine einzelne LED braucht kein DMA
  };

  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "led_strip_new_rmt_device fehlgeschlagen: %s", esp_err_to_name(err));
    return;
  }

  xTaskCreate(watchdog_led_task, "watchdog_led", WATCHDOG_TASK_STACK, NULL, WATCHDOG_TASK_PRIORITY, NULL);
  ESP_LOGI(TAG, "WatchdogManager gestartet (RGB-LED an GPIO%d)", RGB_LED_GPIO);
}
