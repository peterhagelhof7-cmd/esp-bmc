#pragma once

#include <stdbool.h>

// SensorManager (docs/pflichtenheft.txt Abschnitt 3.2) - NTC-Auswertung
// ueber Spannungsteiler, DHT11-Auswertung, Schwellwert-Vergleich gegen
// ConfigManager-Werte. SensorTask laeuft alle 60 Sekunden (Abschnitt 2.1,
// "analog Sensormeter-Familie").
//
// ADC-Kanal/GPIO final festgelegt (2026-07-18) gegen die Vendor-Pinout-
// Bilder und die ESP-IDF-SoC-Header abgeglichen (keine Kollision mit
// Strapping-Pins/JTAG/UART/USB) - siehe docs/verdrahtungsplan.html und
// docs/entscheidungen.md. Noch nicht auf echter Hardware verifiziert (kein
// Board vorhanden), aber kein reiner Wokwi-Platzhalter mehr.

#define SENSOR_MANAGER_NTC_ADC_CHANNEL 0  // ADC1_CH0 = GPIO1
#define SENSOR_MANAGER_DHT_GPIO 2         // Eingang/Ausgang, 1-Wire-artig (DHT11)

// Konfiguriert ADC + DHT-Pin und startet die SensorTask (60s-Intervall).
// Einmalig aus app_main() aufrufen.
void sensor_manager_init(void);

// Zuletzt gueltig gelesene Werte. Rueckgabe false, wenn noch nie ein
// plausibler Messwert vorlag (siehe Abschnitt 8.3 - implausible Messwerte
// werden verworfen/markiert, loesen aber selbst keine Benachrichtigung aus).
bool sensor_manager_get_ntc_temp_c(float* out_value);
bool sensor_manager_get_dht_temp_c(float* out_value);
bool sensor_manager_get_dht_humidity_pct(float* out_value);
