#include "sensor_manager.h"

#include <math.h>
#include <string.h>

#include "config_manager.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "notification_manager.h"
#include "sensor_history.h"

static const char* TAG = "sensor_manager";

#define SENSOR_TASK_INTERVAL_MS (60 * 1000)

// NTC 10K B3590 (docs/bom.md) an Spannungsteiler: 3V3 -> Festwiderstand
// (SERIES_OHM) -> ADC-Pin -> NTC -> GND. Steigt die Temperatur, sinkt
// Rntc und damit auch Vadc.
#define NTC_SERIES_OHM 10000.0f
#define NTC_NOMINAL_OHM 10000.0f
#define NTC_NOMINAL_TEMP_C 25.0f
#define NTC_BETA 3590.0f
#define ADC_VCC_MV 3300.0f

// Plausibilitaetsgrenzen (Abschnitt 8.3 - implausible Werte verwerfen, keine
// Benachrichtigung aus einem Plausibilitaetsfehler selbst ausloesen).
#define NTC_TEMP_MIN_C -20.0f
#define NTC_TEMP_MAX_C 100.0f
#define DHT_TEMP_MIN_C 0.0f
#define DHT_TEMP_MAX_C 50.0f
#define DHT_HUMIDITY_MIN_PCT 20.0f
#define DHT_HUMIDITY_MAX_PCT 90.0f

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;

static bool s_ntc_valid;
static float s_ntc_temp_c;
static bool s_dht_valid;
static float s_dht_temp_c;
static float s_dht_humidity_pct;

// ---------------------------------------------------------------------
// NTC (ADC + Beta-Formel)
// ---------------------------------------------------------------------

static void ntc_adc_init(void) {
  adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

  adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, SENSOR_MANAGER_NTC_ADC_CHANNEL, &chan_cfg));

  // ESP32-S3 unterstuetzt das Curve-Fitting-Kalibrierschema durchgehend -
  // ohne Kalibrierung waeren die mV-Werte fuer die Spannungsteiler-Rechnung
  // zu ungenau.
  adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = ADC_UNIT_1,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle));
}

// Liefert false bei implausiblem Rohwert (offener/kurzgeschlossener Fuehler
// draengt Vadc gegen 0 bzw. VCC).
static bool ntc_read_temp_c(float* out_temp_c) {
  int raw;
  ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, SENSOR_MANAGER_NTC_ADC_CHANNEL, &raw));
  int mv;
  ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &mv));

  float margin_mv = ADC_VCC_MV * 0.02f;  // 2% Rand gegen Divide-by-Zero/Kurzschlusserkennung
  if (mv < margin_mv || mv > (ADC_VCC_MV - margin_mv)) {
    ESP_LOGW(TAG, "NTC: Vadc=%d mV ausserhalb des plausiblen Spannungsteiler-Bereichs", mv);
    return false;
  }

  float r_ntc = NTC_SERIES_OHM * mv / (ADC_VCC_MV - mv);
  float steinhart = logf(r_ntc / NTC_NOMINAL_OHM) / NTC_BETA;
  steinhart += 1.0f / (NTC_NOMINAL_TEMP_C + 273.15f);
  float temp_c = (1.0f / steinhart) - 273.15f;

  if (temp_c < NTC_TEMP_MIN_C || temp_c > NTC_TEMP_MAX_C) {
    ESP_LOGW(TAG, "NTC: Temperatur %.1f C ausserhalb des plausiblen Bereichs", temp_c);
    return false;
  }
  *out_temp_c = temp_c;
  return true;
}

// ---------------------------------------------------------------------
// DHT11 (bit-gebankte 1-Wire-artige Auswertung, kein Framework-Treiber -
// ESP-IDF bringt keinen DHT-Treiber mit, das Protokoll ist einfach genug
// fuer eine eigene Implementierung ohne externe Abhaengigkeit).
// ---------------------------------------------------------------------

static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

// Wartet bis zu timeout_us auf den angegebenen Pegel. false bei Timeout.
static bool dht_wait_for_level(int level, int timeout_us) {
  int64_t start = esp_timer_get_time();
  while (gpio_get_level(SENSOR_MANAGER_DHT_GPIO) != level) {
    if ((esp_timer_get_time() - start) > timeout_us) return false;
  }
  return true;
}

// Liest die rohen 5 Bytes (RH_int, RH_dec, T_int, T_dec, Checksumme).
// false bei Timeout/Verbindungsfehler - Checksummenpruefung erfolgt separat.
static bool dht_read_raw(uint8_t data[5]) {
  memset(data, 0, 5);

  gpio_set_direction(SENSOR_MANAGER_DHT_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(SENSOR_MANAGER_DHT_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(20));  // Start-Signal: Host zieht Leitung >=18ms LOW
  gpio_set_level(SENSOR_MANAGER_DHT_GPIO, 1);
  esp_rom_delay_us(30);
  gpio_set_direction(SENSOR_MANAGER_DHT_GPIO, GPIO_MODE_INPUT);

  // Ab hier ist Timing eng (Bit-Fenster ~70-120us) - Interrupts fuer die
  // Dauer des Auslesens sperren, damit der FreeRTOS-Tick nicht dazwischenfunkt.
  taskENTER_CRITICAL(&s_dht_mux);

  bool ok = dht_wait_for_level(0, 100) &&  // Sensor-Antwort: 80us LOW
            dht_wait_for_level(1, 100) &&  // dann 80us HIGH
            dht_wait_for_level(0, 100);    // dann Start des ersten Datenbits

  for (int i = 0; ok && i < 40; i++) {
    if (!dht_wait_for_level(1, 100)) {
      ok = false;
      break;
    }
    int64_t t0 = esp_timer_get_time();
    if (!dht_wait_for_level(0, 100)) {
      ok = false;
      break;
    }
    int64_t high_duration_us = esp_timer_get_time() - t0;
    data[i / 8] <<= 1;
    if (high_duration_us > 40) data[i / 8] |= 1;  // ~26-28us=0, ~70us=1
  }

  taskEXIT_CRITICAL(&s_dht_mux);
  return ok;
}

static bool dht_read(float* out_temp_c, float* out_humidity_pct) {
  uint8_t data[5];
  if (!dht_read_raw(data)) {
    ESP_LOGW(TAG, "DHT: kein/unvollstaendiges Antwortsignal (Timeout)");
    return false;
  }

  uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
  if (checksum != data[4]) {
    ESP_LOGW(TAG, "DHT: Pruefsummenfehler");
    return false;
  }

  float humidity_pct;
  float temp_c;
#if CONFIG_ESP_BMC_SENSOR_DHT_IS_DHT22_FORMAT
  // Nur fuer die Wokwi-Simulation: der dort verfuegbare "wokwi-dht22"-Part
  // ist protokoll-/pruefsummenkompatibel, nutzt aber das DHT22-Datenformat
  // (0.1-Aufloesung + Vorzeichenbit) statt des DHT11-Ganzzahlformats -
  // siehe docs/entscheidungen.md P3 und Kconfig-Hilfetext.
  humidity_pct = (float)((data[0] << 8) | data[1]) / 10.0f;
  int16_t temp_raw = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
  temp_c = temp_raw / 10.0f;
  if (data[2] & 0x80) temp_c = -temp_c;
#else
  // DHT11-Format: Integer-Anteil in data[0]/data[2], Dezimal-Anteil bei
  // Original-DHT11 immer 0, manche Klone nutzen ihn dennoch (0.1-Aufloesung
  // + Vorzeichenbit analog DHT22) - deshalb hier ebenfalls ausgewertet statt
  // stillschweigend verworfen.
  humidity_pct = data[0] + (data[1] / 10.0f);
  temp_c = data[2] + ((data[3] & 0x7F) / 10.0f);
  if (data[3] & 0x80) temp_c = -temp_c;
#endif

  if (temp_c < DHT_TEMP_MIN_C || temp_c > DHT_TEMP_MAX_C ||
      humidity_pct < DHT_HUMIDITY_MIN_PCT || humidity_pct > DHT_HUMIDITY_MAX_PCT) {
    ESP_LOGW(TAG, "DHT: Messwert ausserhalb des plausiblen Bereichs (T=%.1f C, RH=%.1f %%)", temp_c,
             humidity_pct);
    return false;
  }

  *out_temp_c = temp_c;
  *out_humidity_pct = humidity_pct;
  return true;
}

// ---------------------------------------------------------------------
// SensorTask
// ---------------------------------------------------------------------

static void check_threshold(const char* quelle, bool valid, float value, float threshold) {
  if (valid && value > threshold) {
    notification_manager_trigger(quelle, value, threshold);
  }
}

static void sensor_manager_task(void* arg) {
  (void)arg;
  for (;;) {
    float ntc_temp_c;
    s_ntc_valid = ntc_read_temp_c(&ntc_temp_c);
    if (s_ntc_valid) {
      s_ntc_temp_c = ntc_temp_c;
      ESP_LOGI(TAG, "NTC: %.1f C", ntc_temp_c);
      check_threshold("NTC-Temperatur", true, ntc_temp_c, config_manager_get_ntc_temp_max_c());
    }

    float dht_temp_c, dht_humidity_pct;
    s_dht_valid = dht_read(&dht_temp_c, &dht_humidity_pct);
    if (s_dht_valid) {
      s_dht_temp_c = dht_temp_c;
      s_dht_humidity_pct = dht_humidity_pct;
      ESP_LOGI(TAG, "DHT11: %.1f C, %.1f %% RH", dht_temp_c, dht_humidity_pct);
      check_threshold("DHT11-Temperatur", true, dht_temp_c, config_manager_get_dht_temp_max_c());
      check_threshold("DHT11-Luftfeuchte", true, dht_humidity_pct,
                       config_manager_get_dht_humidity_max_pct());
    }

    sensor_history_maybe_record(s_ntc_valid, s_ntc_temp_c, s_dht_valid, s_dht_temp_c, s_dht_humidity_pct);

    vTaskDelay(pdMS_TO_TICKS(SENSOR_TASK_INTERVAL_MS));
  }
}

void sensor_manager_init(void) {
  ntc_adc_init();

  gpio_set_level(SENSOR_MANAGER_DHT_GPIO, 1);
  gpio_set_direction(SENSOR_MANAGER_DHT_GPIO, GPIO_MODE_INPUT);

  // Gleiche Prioritaet wie main_task (siehe gpio_manager.c fuer die
  // ausfuehrliche Begruendung - eine hoehere Prioritaet liess main_task in
  // der Wokwi-Simulation nie wieder zum Zug kommen).
  xTaskCreatePinnedToCore(sensor_manager_task, "sensor_manager", 4096, NULL, 1, NULL, 0);
  ESP_LOGI(TAG, "SensorManager gestartet (NTC + DHT11, 60s-Intervall)");
}

bool sensor_manager_get_ntc_temp_c(float* out_value) {
  if (!s_ntc_valid) return false;
  *out_value = s_ntc_temp_c;
  return true;
}

bool sensor_manager_get_dht_temp_c(float* out_value) {
  if (!s_dht_valid) return false;
  *out_value = s_dht_temp_c;
  return true;
}

bool sensor_manager_get_dht_humidity_pct(float* out_value) {
  if (!s_dht_valid) return false;
  *out_value = s_dht_humidity_pct;
  return true;
}
