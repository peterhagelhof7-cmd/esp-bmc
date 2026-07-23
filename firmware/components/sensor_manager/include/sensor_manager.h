#pragma once

#include <stdbool.h>

// SensorManager (docs/pflichtenheft.txt Abschnitt 3.2) - NTC-Auswertung
// ueber Spannungsteiler, DHT11-Auswertung, Schwellwert-Vergleich gegen
// ConfigManager-Werte. SensorTask laeuft alle 60 Sekunden (Abschnitt 2.1,
// "analog Sensormeter-Familie").
//
// ADC-Kanal/GPIO auf die linke Pinleiste umgelegt (2026-07-20), damit eine
// Huckepack-Platine mit nur einer einzigen Stiftleiste auskommt - die acht
// Taster-/LED-Signale in gpio_manager.h (4/5/6/7/15/16/17/18) lagen bereits
// alle auf der linken Leiste, nur NTC (vorher GPIO1) und DHT (vorher GPIO2)
// sassen auf der rechten. Beide jetzt auf zuvor freie linke Pins verschoben,
// gegen die ESP-IDF-SoC-Header abgeglichen (keine Kollision mit
// Strapping-Pins/JTAG/UART/USB) - siehe docs/verdrahtungsplan.html und
// docs/entscheidungen.md. Noch nicht auf echter Hardware verifiziert (kein
// Board vorhanden).

#define SENSOR_MANAGER_NTC_ADC_CHANNEL 8  // ADC1_CH8 = GPIO9 (linke Leiste)
#define SENSOR_MANAGER_DHT_GPIO 11        // GPIO11, Eingang/Ausgang, 1-Wire-artig (DHT11) (linke Leiste)

// Konfiguriert ADC + DHT-Pin und startet die SensorTask (60s-Intervall).
// Einmalig aus app_main() aufrufen.
void sensor_manager_init(void);

// Zuletzt gueltig gelesene Werte. Rueckgabe false, wenn noch nie ein
// plausibler Messwert vorlag (siehe Abschnitt 8.3 - implausible Messwerte
// werden verworfen/markiert, loesen aber selbst keine Benachrichtigung aus).
bool sensor_manager_get_ntc_temp_c(float* out_value);
bool sensor_manager_get_dht_temp_c(float* out_value);
bool sensor_manager_get_dht_humidity_pct(float* out_value);
