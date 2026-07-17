#pragma once

// NotificationManager (Pflichtenheft Abschnitt 3.8) - Schwellwert-
// Ueberschreitung -> Benachrichtigung. Der Versandweg (MQTT/Home-Assistant
// analog Sensormeter-Familie, oder ein anderer Weg) ist bewusst noch offen
// (siehe docs/pflichtenheft.txt Abschnitt 12) - dieser Stand stellt nur den
// Aufrufpunkt bereit, den SensorManager (P3) schon nutzen kann, ohne dem
// spaeteren Versandweg vorzugreifen.

// Wird bei jeder Schwellwert-Ueberschreitung aufgerufen. "quelle" bezeichnet
// die Messgroesse (z.B. "NTC-Temperatur"), value/threshold die konkreten
// Werte fuer die Meldung.
void notification_manager_trigger(const char* quelle, float value, float threshold);
