#include "config_manager.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define DEVICE_CONFIG_FILE "/storage/device.json"
#define DEVICE_NAME_CAP 32
#define DEVICE_TYPE "ESP-BMC"  // fest, identisch fuer jedes Geraet dieser Firmware

static bool s_tastschutz_active = false;

// Default-Schwellwerte, bis eine echte Konfiguration (LittleFS/Webinterface-
// Upload, P5) existiert - grosszuegig ueber ueblichen Buerotemperaturen/
// -feuchten gewaehlt, damit ein unkonfiguriertes Geraet nicht sofort
// Falschalarme ausloest.
static float s_ntc_temp_max_c = 60.0f;
static float s_dht_temp_max_c = 40.0f;
static float s_dht_humidity_max_pct = 70.0f;

static char s_device_name[DEVICE_NAME_CAP] = DEVICE_TYPE;  // Default = Geraetetyp, bis frei benannt

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

// ---------------------------------------------------------------------
// Geraetename - einziges Feld hier mit echter Persistenz (analog
// WLAN/WireGuard/SNMP-Community: JSON auf der storage-Partition).
// ---------------------------------------------------------------------

static void save_device_name(void) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", s_device_name);
  char* text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) return;

  FILE* f = fopen(DEVICE_CONFIG_FILE, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  }
  cJSON_free(text);
}

static void load_device_name(void) {
  FILE* f = fopen(DEVICE_CONFIG_FILE, "r");
  if (!f) return;

  char buf[128];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  cJSON* root = cJSON_Parse(buf);
  if (!root) return;
  cJSON* item = cJSON_GetObjectItem(root, "name");
  if (cJSON_IsString(item)) {
    strncpy(s_device_name, item->valuestring, sizeof(s_device_name) - 1);
  }
  cJSON_Delete(root);
}

void config_manager_init(void) { load_device_name(); }

void config_manager_get_device_name(char* out, size_t out_len) {
  strncpy(out, s_device_name, out_len - 1);
  out[out_len - 1] = '\0';
}

bool config_manager_set_device_name(const char* name) {
  size_t len = strlen(name);
  if (len == 0 || len >= DEVICE_NAME_CAP) return false;
  strncpy(s_device_name, name, sizeof(s_device_name) - 1);
  save_device_name();
  return true;
}

const char* config_manager_get_device_type(void) { return DEVICE_TYPE; }

void config_manager_reset_to_defaults(void) {
  s_tastschutz_active = false;
  s_ntc_temp_max_c = 60.0f;
  s_dht_temp_max_c = 40.0f;
  s_dht_humidity_max_pct = 70.0f;
  strncpy(s_device_name, DEVICE_TYPE, sizeof(s_device_name) - 1);
  s_device_name[sizeof(s_device_name) - 1] = '\0';
  remove(DEVICE_CONFIG_FILE);
}
