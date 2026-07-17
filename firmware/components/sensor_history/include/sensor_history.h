#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SensorHistory (webconfig.txt "Seite Logs": "Download der Sensorwerten
// (24h)") - Ringpuffer mit stuendlichen Werten. Bewusst RAM-only (nicht
// auf der storage-Partition persistiert): 24 kleine Eintraege, ein
// Reset beim Neustart ist fuer diesen Anwendungsfall hinnehmbar und
// spart wiederkehrenden Flash-Verschleiss durch stuendliche Schreibzugriffe.

typedef struct {
  bool ntc_valid;
  float ntc_temp_c;
  bool dht_valid;
  float dht_temp_c;
  float dht_humidity_pct;
  int64_t recorded_at_us;
} sensor_history_entry_t;

void sensor_history_init(void);

// Von sensor_manager einmal je 60s-Zyklus aufgerufen - entscheidet intern
// selbst, ob seit dem letzten Eintrag eine volle Stunde vergangen ist
// (dann wird ein neuer Ringpuffer-Slot beschrieben, sonst passiert nichts).
void sensor_history_maybe_record(bool ntc_valid, float ntc_temp_c, bool dht_valid, float dht_temp_c,
                                  float dht_humidity_pct);

// Schreibt die aktuelle Historie als CSV (Header + bis zu 24 Zeilen,
// aeltester Eintrag zuerst) nach out. Liefert die Anzahl geschriebener
// Zeichen (wie snprintf).
size_t sensor_history_get_csv(char* out, size_t out_len);

// Wie sensor_history_get_csv(), aber als rohe Eintraege (aeltester
// zuerst) statt Text - fuer die 24h-Chart-Darstellung
// (webconfig.txt Uebersichtsseite, "sensor werte in einem chart ...
// 24h ..., stuendliche werte"). Schreibt bis zu max_entries nach out,
// liefert die tatsaechliche Anzahl.
size_t sensor_history_get_entries(sensor_history_entry_t* out, size_t max_entries);

// Leert den Ringpuffer (webconfig.txt "Seite Einstellungen": "reset
// (... einstellungen und werte)" - dies ist der "Werte"-Teil).
void sensor_history_reset(void);
