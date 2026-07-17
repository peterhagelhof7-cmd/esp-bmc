#include "config_manager.h"

static bool s_tastschutz_active = false;

// Default-Schwellwerte, bis eine echte Konfiguration (LittleFS/Webinterface-
// Upload, P5) existiert - grosszuegig ueber ueblichen Buerotemperaturen/
// -feuchten gewaehlt, damit ein unkonfiguriertes Geraet nicht sofort
// Falschalarme ausloest.
static float s_ntc_temp_max_c = 60.0f;
static float s_dht_temp_max_c = 40.0f;
static float s_dht_humidity_max_pct = 70.0f;

bool config_manager_is_tastschutz_active(void) {
  return s_tastschutz_active;
}

void config_manager_set_tastschutz(bool active) {
  s_tastschutz_active = active;
}

float config_manager_get_ntc_temp_max_c(void) { return s_ntc_temp_max_c; }
void config_manager_set_ntc_temp_max_c(float value) { s_ntc_temp_max_c = value; }

float config_manager_get_dht_temp_max_c(void) { return s_dht_temp_max_c; }
void config_manager_set_dht_temp_max_c(float value) { s_dht_temp_max_c = value; }

float config_manager_get_dht_humidity_max_pct(void) { return s_dht_humidity_max_pct; }
void config_manager_set_dht_humidity_max_pct(float value) { s_dht_humidity_max_pct = value; }

void config_manager_reset_to_defaults(void) {
  s_tastschutz_active = false;
  s_ntc_temp_max_c = 60.0f;
  s_dht_temp_max_c = 40.0f;
  s_dht_humidity_max_pct = 70.0f;
}
