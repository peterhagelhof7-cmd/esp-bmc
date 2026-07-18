#include "ota_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "firmware_version.h"

static const char* TAG = "ota_manager";

#define MARKER_PREFIX "ESPBMC-FW-ID:"
#define MARKER_PREFIX_LEN (sizeof(MARKER_PREFIX) - 1)
#define MARKER_SUFFIX ":ESPBMC-FW-END"
#define MARKER_SUFFIX_LEN (sizeof(MARKER_SUFFIX) - 1)
#define CAPTURE_CAP 128  // > groesstmoegliches "<PROJECT_ID>:<VERSION>"
#define TAIL_CAP 16      // > MARKER_PREFIX_LEN - 1
#define JOIN_CAP (TAIL_CAP + 1024)

// Nur ueber ota_manager_init()'s ESP_LOGI referenziert, damit der Linker
// ihn nicht als unbenutzt wegoptimiert (der eigentliche Zweck ist, dass
// dieser Byte-String im fertigen .bin auffindbar ist).
static const char s_identity_marker[] =
    MARKER_PREFIX FIRMWARE_PROJECT_ID ":" DEVICE_FIRMWARE_VERSION MARKER_SUFFIX;

static esp_ota_handle_t s_ota_handle;
static const esp_partition_t* s_update_partition;
static bool s_allow_downgrade;
static bool s_marker_found;
static bool s_identity_matches;
static bool s_version_allowed;

static bool s_capturing;
static uint8_t s_tail_buf[TAIL_CAP];
static size_t s_tail_len;
static uint8_t s_capture_buf[CAPTURE_CAP];
static size_t s_capture_len;
static uint8_t s_join_buf[JOIN_CAP];

// Byte-sichere Teilstring-Suche (memmem ist auf picolibc nicht garantiert
// verfuegbar) - bricht im Unterschied zu strstr() NICHT am ersten
// eingebetteten Null-Byte ab.
static int find_bytes(const uint8_t* haystack, size_t haystack_len, const char* needle, size_t needle_len) {
  if (needle_len == 0 || haystack_len < needle_len) return -1;
  for (size_t i = 0; i + needle_len <= haystack_len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0) return (int)i;
  }
  return -1;
}

// Zerlegt "MAJOR.MINOR.PATCH[-SUFFIX]" - kein vollstaendiger Semver-Parser
// (z.B. keine Build-Metadaten "+..."), deckt aber das in diesem Projekt
// genutzte a.b.c[-rcN]-Schema ab (identisch zum Sensormeter-Schema).
static void parse_version(const char* v, int* major, int* minor, int* patch, char* suffix, size_t suffix_cap) {
  *major = *minor = *patch = 0;
  suffix[0] = '\0';

  const char* dash = strchr(v, '-');
  char core[32];
  size_t core_len = dash ? (size_t)(dash - v) : strlen(v);
  if (core_len >= sizeof(core)) core_len = sizeof(core) - 1;
  memcpy(core, v, core_len);
  core[core_len] = '\0';
  if (dash) {
    strncpy(suffix, dash + 1, suffix_cap - 1);
    suffix[suffix_cap - 1] = '\0';
  }

  sscanf(core, "%d.%d.%d", major, minor, patch);
}

// <0/0/>0 wie strcmp. Bei gleichem a.b.c hat "kein Suffix" Vorrang vor
// "mit Suffix" (ein Release gilt als neuer als jede Vorabversion derselben
// Kernversion), bei zwei Suffixen entscheidet der lexikografische
// Vergleich (deckt "rc3" < "rc4" ab) - identisches Schema wie Sensormeter.
static int compare_versions(const char* a, const char* b) {
  int a_major, a_minor, a_patch, b_major, b_minor, b_patch;
  char a_suffix[32] = "", b_suffix[32] = "";
  parse_version(a, &a_major, &a_minor, &a_patch, a_suffix, sizeof(a_suffix));
  parse_version(b, &b_major, &b_minor, &b_patch, b_suffix, sizeof(b_suffix));
  if (a_major != b_major) return a_major - b_major;
  if (a_minor != b_minor) return a_minor - b_minor;
  if (a_patch != b_patch) return a_patch - b_patch;
  if (a_suffix[0] == '\0' && b_suffix[0] != '\0') return 1;
  if (a_suffix[0] != '\0' && b_suffix[0] == '\0') return -1;
  return strcmp(a_suffix, b_suffix);
}

static void handle_marker_payload(const uint8_t* payload, size_t len) {
  int sep = find_bytes(payload, len, ":", 1);
  if (sep < 0) return;

  char project_id[48];
  size_t id_len = (size_t)sep < sizeof(project_id) - 1 ? (size_t)sep : sizeof(project_id) - 1;
  memcpy(project_id, payload, id_len);
  project_id[id_len] = '\0';

  char version[48];
  size_t ver_off = (size_t)sep + 1;
  size_t ver_len = len > ver_off ? len - ver_off : 0;
  if (ver_len >= sizeof(version)) ver_len = sizeof(version) - 1;
  memcpy(version, payload + ver_off, ver_len);
  version[ver_len] = '\0';

  s_marker_found = true;
  s_identity_matches = strcmp(project_id, FIRMWARE_PROJECT_ID) == 0;
  s_version_allowed = s_allow_downgrade || compare_versions(version, DEVICE_FIRMWARE_VERSION) >= 0;
  ESP_LOGI(TAG, "Marker gefunden: Projekt=\"%s\" Version=\"%s\" (Identitaet %s, Version %s)", project_id, version,
           s_identity_matches ? "ok" : "FALSCH", s_version_allowed ? "erlaubt" : "ABGELEHNT");
}

// Sucht ueber Chunk-Grenzen hinweg nach MARKER_PREFIX, faengt danach alles
// bis MARKER_SUFFIX ab. Byte-sicher (memcmp via find_bytes), NICHT
// string-basiert - siehe ota_manager.h fuer den Sensormeter-Bug, den das
// vermeidet.
static void scan_chunk_for_marker(const uint8_t* data, size_t len) {
  if (!s_capturing) {
    size_t offset = 0;
    if (s_tail_len > 0) {
      memcpy(s_join_buf, s_tail_buf, s_tail_len);
      offset = s_tail_len;
    }
    size_t copy_len = len;
    if (offset + copy_len > JOIN_CAP) copy_len = JOIN_CAP - offset;
    memcpy(s_join_buf + offset, data, copy_len);
    size_t join_len = offset + copy_len;

    int prefix_pos = find_bytes(s_join_buf, join_len, MARKER_PREFIX, MARKER_PREFIX_LEN);
    if (prefix_pos < 0) {
      size_t keep = MARKER_PREFIX_LEN > 0 ? MARKER_PREFIX_LEN - 1 : 0;
      if (keep > TAIL_CAP) keep = TAIL_CAP;
      size_t start = join_len > keep ? join_len - keep : 0;
      s_tail_len = join_len - start;
      memcpy(s_tail_buf, s_join_buf + start, s_tail_len);
      return;
    }
    s_capturing = true;
    size_t after_prefix = (size_t)prefix_pos + MARKER_PREFIX_LEN;
    size_t remaining = join_len - after_prefix;
    if (remaining > CAPTURE_CAP) remaining = CAPTURE_CAP;
    memcpy(s_capture_buf, s_join_buf + after_prefix, remaining);
    s_capture_len = remaining;
    s_tail_len = 0;
  } else {
    size_t copy_len = len;
    if (s_capture_len + copy_len > CAPTURE_CAP) copy_len = CAPTURE_CAP - s_capture_len;
    memcpy(s_capture_buf + s_capture_len, data, copy_len);
    s_capture_len += copy_len;
  }

  int suffix_pos = find_bytes(s_capture_buf, s_capture_len, MARKER_SUFFIX, MARKER_SUFFIX_LEN);
  if (suffix_pos >= 0) {
    handle_marker_payload(s_capture_buf, (size_t)suffix_pos);
    s_capturing = false;
    s_capture_len = 0;
    s_tail_len = 0;
    return;
  }
  if (s_capture_len >= CAPTURE_CAP) {
    // Kein gueltiger Marker in plausibler Laenge - wie "nicht gefunden"
    // behandeln, nicht endlos weiter aufsammeln.
    s_capturing = false;
    s_capture_len = 0;
    s_tail_len = 0;
  }
}

void ota_manager_init(void) { ESP_LOGI(TAG, "OtaManager bereit (%s)", s_identity_marker); }

bool ota_manager_begin(bool allow_downgrade) {
  s_allow_downgrade = allow_downgrade;
  s_marker_found = false;
  s_identity_matches = false;
  s_version_allowed = false;
  s_capturing = false;
  s_tail_len = 0;
  s_capture_len = 0;

  s_update_partition = esp_ota_get_next_update_partition(NULL);
  if (!s_update_partition) {
    ESP_LOGE(TAG, "Keine OTA-Zielpartition gefunden");
    return false;
  }
  esp_err_t err = esp_ota_begin(s_update_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin fehlgeschlagen: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "OTA-Upload gestartet -> Partition \"%s\"", s_update_partition->label);
  return true;
}

bool ota_manager_write_chunk(const uint8_t* data, size_t len) {
  if (!s_marker_found) scan_chunk_for_marker(data, len);
  esp_err_t err = esp_ota_write(s_ota_handle, data, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write fehlgeschlagen: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool ota_manager_end(void) {
  if (!s_marker_found || !s_identity_matches || !s_version_allowed) {
    ESP_LOGW(TAG, "OTA verworfen (Marker=%d Identitaet=%d Version=%d)", s_marker_found, s_identity_matches,
             s_version_allowed);
    esp_ota_abort(s_ota_handle);
    return false;
  }
  esp_err_t err = esp_ota_end(s_ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end fehlgeschlagen: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_ota_set_boot_partition(s_update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition fehlgeschlagen: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "OTA erfolgreich, Boot-Partition auf \"%s\" gesetzt", s_update_partition->label);
  return true;
}

bool ota_manager_marker_found(void) { return s_marker_found; }
bool ota_manager_identity_matches(void) { return s_identity_matches; }
bool ota_manager_version_allowed(void) { return s_version_allowed; }

const char* ota_manager_get_version(void) { return DEVICE_FIRMWARE_VERSION; }
