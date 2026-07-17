#include "audit_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "time_manager.h"

#define AUDIT_LOG_FILE "/storage/audit.log"
#define AUDIT_LOG_FILE_OLD "/storage/audit.log.old"
#define AUDIT_LOG_ROTATE_SIZE (16 * 1024)

static const char* TAG = "audit_log";

static long file_size(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fclose(f);
  return size;
}

void audit_log_init(void) { ESP_LOGI(TAG, "AuditLog bereit (%s)", AUDIT_LOG_FILE); }

void audit_log_add(const char* event) {
  if (file_size(AUDIT_LOG_FILE) > AUDIT_LOG_ROTATE_SIZE) {
    remove(AUDIT_LOG_FILE_OLD);
    rename(AUDIT_LOG_FILE, AUDIT_LOG_FILE_OLD);
  }

  FILE* f = fopen(AUDIT_LOG_FILE, "a");
  if (!f) {
    ESP_LOGE(TAG, "Konnte %s nicht oeffnen", AUDIT_LOG_FILE);
    return;
  }

  if (time_manager_is_synced()) {
    time_t now = time(NULL);
    char timestamp[32];
    ctime_r(&now, timestamp);
    timestamp[strcspn(timestamp, "\n")] = '\0';  // ctime_r haengt selbst ein '\n' an
    fprintf(f, "[%s] %s\n", timestamp, event);
  } else {
    fprintf(f, "[Uptime %llds] %s\n", (long long)(esp_timer_get_time() / 1000000), event);
  }
  fclose(f);
}

size_t audit_log_read(char* out, size_t out_len) {
  FILE* f = fopen(AUDIT_LOG_FILE, "r");
  if (!f) {
    out[0] = '\0';
    return 0;
  }
  size_t n = fread(out, 1, out_len - 1, f);
  fclose(f);
  out[n] = '\0';
  return n;
}
