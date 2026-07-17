#pragma once

#include <stdbool.h>

// Minimaler Platzstand fuer P0/P2 (siehe docs/implementierungsplan.html).
// Haelt aktuell nur den Tastschutz-Zustand rein im RAM - Persistenz
// (NVS/LittleFS, WLAN-Zugangsdaten, WireGuard-Konfiguration, Schwellwerte)
// ist laut docs/pflichtenheft.txt Abschnitt 4 noch offen und folgt in einer
// spaeteren Ausbaustufe (P0-Nachtrag oder eigene Phase).

// Tastschutz-Zustand abfragen (siehe docs/lastenheft.txt Abschnitt 10.4,
// Schritt 2: "Firmware prueft die Tastschutz-Bedingung aus dem
// Webinterface").
bool config_manager_is_tastschutz_active(void);

// Tastschutz setzen - spaeter vom WebServerManager aufgerufen, sobald das
// Webinterface existiert (P5). Fuer P0/P2 und die Wokwi-Simulation genuegt
// der reine Getter/Setter ohne Persistenz.
void config_manager_set_tastschutz(bool active);

// Schwellwerte je Messgroesse (Pflichtenheft Abschnitt 3.2/3.3 und 4.1) -
// bei Ueberschreitung loest SensorManager eine Benachrichtigung aus.
// Gleiches Platzstand-Muster wie Tastschutz oben: RAM-only, keine
// LittleFS-Persistenz vor der eigenen Config-Phase.
float config_manager_get_ntc_temp_max_c(void);
void config_manager_set_ntc_temp_max_c(float value);

float config_manager_get_dht_temp_max_c(void);
void config_manager_set_dht_temp_max_c(float value);

float config_manager_get_dht_humidity_max_pct(void);
void config_manager_set_dht_humidity_max_pct(float value);

// Setzt Tastschutz und alle Schwellwerte auf die eingebauten Default-Werte
// zurueck (webconfig.txt "Seite Einstellungen": "reset (nur einstellungen,
// oder einstellungen und werte)" - dies ist der "Einstellungen"-Teil).
void config_manager_reset_to_defaults(void);
