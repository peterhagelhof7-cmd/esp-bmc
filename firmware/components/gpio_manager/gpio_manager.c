#include "gpio_manager.h"

#include <stdio.h>

#include "audit_log.h"
#include "config_manager.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "gpio_manager";

// Entprellung: eine Pin-Aenderung zaehlt erst nach dieser vielen
// aufeinanderfolgenden, uebereinstimmenden Poll-Zyklen als stabil (siehe
// docs/pflichtenheft.txt Abschnitt 9, "Entprellung im ms-Bereich").
#define DEBOUNCE_POLL_MS 5
#define DEBOUNCE_STABLE_CYCLES 6  // 6 * 5ms = 30ms

// Dauer eines per Software ausgeloesten Tastendrucks (uebliche
// PC-Konvention: kurzer Druck = Soft-Power, langer Druck = erzwungenes
// Abschalten).
#define REMOTE_PRESS_PUSH_MS 300
#define REMOTE_PRESS_HOLD_MS 5000

typedef struct {
  const char* name;  // fuer AuditLog-Eintraege ("Power"/"Reset")
  int sense_gpio;
  int drive_gpio;
  bool debounced_pressed;
  int stable_count;
  bool last_raw;
  // Software-Schattenkopie des Weiterleitungs-Zustands: gpio_get_level()
  // auf einem als OUTPUT konfigurierten Pin ist kein zuverlaessiges
  // Register-Readback (in der Wokwi-Simulation blieb das Ergebnis nach
  // dem Weiterleiten dauerhaft auf "aktiv" haengen, unabhaengig vom
  // tatsaechlich gesetzten Pegel). Der Zustand, den wir selbst setzen,
  // wird deshalb direkt gehalten statt zurueckgelesen.
  bool weitergeleitet;
  // Per Software ausgeloester Tastendruck (gpio_manager_trigger_*) - 0 =
  // inaktiv, sonst esp_timer_get_time()-Zeitstempel, bis zu dem noch
  // weitergeleitet werden soll.
  int64_t remote_release_at_us;
} TasterKanal;

static TasterKanal s_power = {
    .name = "Power", .sense_gpio = GPIO_REMOTE_POWER_SENSE, .drive_gpio = GPIO_REMOTE_POWER_DRIVE};
static TasterKanal s_reset = {
    .name = "Reset", .sense_gpio = GPIO_REMOTE_RESET_SENSE, .drive_gpio = GPIO_REMOTE_RESET_DRIVE};

#define HDD_LED_RECENT_WINDOW_US (10LL * 1000000LL)
// Weit in der Vergangenheit initialisiert, damit "in den letzten 10s aktiv"
// direkt nach dem Boot korrekt false liefert, bevor je ein Signal anlag.
static int64_t s_hdd_led_last_active_us = -HDD_LED_RECENT_WINDOW_US * 2;

static void configure_input_pullup(int gpio_num) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << gpio_num,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

static void configure_output(int gpio_num, bool initial_high) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << gpio_num,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(gpio_num, initial_high ? 1 : 0);
}

// Ein Entprellungsschritt fuer einen Taster-Kanal - implementiert den
// Ablauf aus docs/lastenheft.txt Abschnitt 10.4 komplett:
//   1. Taster gedrueckt (sense_gpio LOW) wird hier erkannt
//   2. Tastschutz-Abfrage
//   3. Weiterleitung auf drive_gpio, solange gedrueckt UND kein Tastschutz
static void taster_kanal_poll(TasterKanal* k) {
  bool raw_pressed = (gpio_get_level(k->sense_gpio) == 0);  // aktiv LOW

  if (raw_pressed == k->last_raw) {
    if (k->stable_count < DEBOUNCE_STABLE_CYCLES) k->stable_count++;
  } else {
    k->stable_count = 0;
    k->last_raw = raw_pressed;
  }

  if (k->stable_count >= DEBOUNCE_STABLE_CYCLES && k->debounced_pressed != raw_pressed) {
    k->debounced_pressed = raw_pressed;
    ESP_LOGI(TAG, "GPIO%d: Taster-Erfassung -> %s", k->sense_gpio,
             k->debounced_pressed ? "gedrueckt" : "losgelassen");
    if (k->debounced_pressed) {
      char event[48];
      snprintf(event, sizeof(event), "Physischer %s-Taster betaetigt", k->name);
      audit_log_add(event);
    }
  }

  // Ein per Software ausgeloester Tastendruck (gpio_manager_trigger_*)
  // zaehlt genauso wie ein physischer, bis seine Dauer abgelaufen ist.
  bool remote_pressed = k->remote_release_at_us != 0;
  if (remote_pressed && esp_timer_get_time() >= k->remote_release_at_us) {
    k->remote_release_at_us = 0;
    remote_pressed = false;
  }

  // Schritt 2+3: nur weiterleiten, wenn (entprellt oder per Software)
  // gedrueckt UND kein Tastschutz aktiv - active-LOW auf der
  // Optokoppler-Ausgangsseite angenommen (GPIO sinkt Strom durch die
  // PC817-Eingangs-LED, siehe docs/bom.md), wie auf der Erfassungsseite.
  bool tastschutz = config_manager_is_tastschutz_active();
  bool soll_weiterleiten = (k->debounced_pressed || remote_pressed) && !tastschutz;
  gpio_set_level(k->drive_gpio, soll_weiterleiten ? 0 : 1);
  k->weitergeleitet = soll_weiterleiten;
}

// Startet einen per Software ausgeloesten Tastendruck - false, wenn
// Tastschutz aktiv ist (dann wird bewusst nichts ausgeloest, auch keine
// verzoegerte Wirkung nach Aufheben des Tastschutzes).
static bool trigger_kanal(TasterKanal* k, uint32_t duration_ms) {
  if (config_manager_is_tastschutz_active()) return false;
  k->remote_release_at_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
  return true;
}

static void gpio_manager_task(void* arg) {
  (void)arg;
  for (;;) {
    taster_kanal_poll(&s_power);
    taster_kanal_poll(&s_reset);
    if (gpio_get_level(GPIO_REMOTE_HDD_LED_IN) == 0) {  // aktiv LOW, wie gpio_manager_read_hdd_led()
      s_hdd_led_last_active_us = esp_timer_get_time();
    }
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS));
  }
}

void gpio_manager_init(void) {
  configure_input_pullup(GPIO_REMOTE_POWER_SENSE);
  configure_input_pullup(GPIO_REMOTE_RESET_SENSE);
  configure_output(GPIO_REMOTE_POWER_DRIVE, /*initial_high=*/true);  // inaktiv beim Start
  configure_output(GPIO_REMOTE_RESET_DRIVE, /*initial_high=*/true);

  configure_input_pullup(GPIO_REMOTE_POWER_LED_IN);
  configure_input_pullup(GPIO_REMOTE_HDD_LED_IN);
  configure_output(GPIO_REMOTE_POWER_LED_OUT, /*initial_high=*/false);
  configure_output(GPIO_REMOTE_HDD_LED_OUT, /*initial_high=*/false);

  // Prioritaet 1 (= main_task, nicht hoeher): in der Wokwi-Simulation
  // kehrte main_task nach xTaskCreate() nie mehr zurueck, sobald der neue
  // Task eine hoehere Prioritaet hatte und main_task dadurch sofort
  // verdraengt wurde (main_task wurde danach nie wieder eingeplant -
  // reproduzierbar per Debug-Logging isoliert). Gleiche Prioritaet
  // vermeidet die sofortige Verdraengung bei der Task-Erstellung; fuer
  // dieses reine Polling (5ms-Zyklus, keine Echtzeit-Anforderung) ist das
  // auch auf echter Hardware unproblematisch.
  xTaskCreatePinnedToCore(gpio_manager_task, "gpio_manager", 2048, NULL, 1, NULL, 0);
  ESP_LOGI(TAG, "GpioManager gestartet (Taster-Erfassung + Weiterleitung, LED-Ein/Ausgaenge)");
}

bool gpio_manager_power_taste_gedrueckt(void) { return s_power.debounced_pressed; }
bool gpio_manager_reset_taste_gedrueckt(void) { return s_reset.debounced_pressed; }

bool gpio_manager_power_taste_weitergeleitet(void) { return s_power.weitergeleitet; }
bool gpio_manager_reset_taste_weitergeleitet(void) { return s_reset.weitergeleitet; }

bool gpio_manager_read_power_led(void) { return gpio_get_level(GPIO_REMOTE_POWER_LED_IN) == 0; }
bool gpio_manager_read_hdd_led(void) { return gpio_get_level(GPIO_REMOTE_HDD_LED_IN) == 0; }

void gpio_manager_set_power_led(bool on) { gpio_set_level(GPIO_REMOTE_POWER_LED_OUT, on ? 1 : 0); }
void gpio_manager_set_hdd_led(bool on) { gpio_set_level(GPIO_REMOTE_HDD_LED_OUT, on ? 1 : 0); }

bool gpio_manager_trigger_power(bool hold) {
  return trigger_kanal(&s_power, hold ? REMOTE_PRESS_HOLD_MS : REMOTE_PRESS_PUSH_MS);
}

bool gpio_manager_trigger_reset(void) { return trigger_kanal(&s_reset, REMOTE_PRESS_PUSH_MS); }

bool gpio_manager_hdd_led_active_recently(void) {
  return (esp_timer_get_time() - s_hdd_led_last_active_us) < HDD_LED_RECENT_WINDOW_US;
}
