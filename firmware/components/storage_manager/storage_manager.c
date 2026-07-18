#include "storage_manager.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "storage_manager";
static const char* PARTITION_LABEL = "storage";
#define STORAGE_BASE_PATH "/storage"

static bool s_mounted = false;

void storage_manager_init(void) {
  // format_if_mount_failed=true: eine frisch geflashte storage-Partition
  // ist unformatiert, der erste Mount nach dem Flashen (oder nach einem
  // Partitions-Erase) formatiert sie einmalig und dauert dadurch
  // spuerbar laenger als jeder folgende Boot (blockiert app_main() vor
  // usb_manager_init() - siehe docs/entscheidungen.md "Hinweis: erster
  // Boot nach dem Flashen dauert laenger").
  esp_vfs_littlefs_conf_t conf = {
      .base_path = STORAGE_BASE_PATH,
      .partition_label = PARTITION_LABEL,
      .format_if_mount_failed = true,
      .dont_mount = false,
  };

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Mount fehlgeschlagen (%s)", esp_err_to_name(err));
    return;
  }
  s_mounted = true;

  size_t total_bytes = 0, used_bytes = 0;
  esp_littlefs_info(PARTITION_LABEL, &total_bytes, &used_bytes);
  ESP_LOGI(TAG, "StorageManager gemountet unter %s (%u/%u Byte belegt)", STORAGE_BASE_PATH,
           (unsigned)used_bytes, (unsigned)total_bytes);
}

bool storage_manager_is_mounted(void) { return s_mounted; }

const char* storage_manager_base_path(void) { return STORAGE_BASE_PATH; }

bool storage_manager_get_usage(size_t* out_used_bytes, size_t* out_total_bytes) {
  if (!s_mounted) return false;
  return esp_littlefs_info(PARTITION_LABEL, out_total_bytes, out_used_bytes) == ESP_OK;
}
