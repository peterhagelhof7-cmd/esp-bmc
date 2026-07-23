#include "sensor_history.h"

#include <stdio.h>

#include "esp_timer.h"

#define HISTORY_SLOTS SENSOR_HISTORY_SLOTS
#define RECORD_INTERVAL_US (5LL * 60LL * 1000000LL)  // alle 5 Minuten ein Eintrag

static sensor_history_entry_t s_history[HISTORY_SLOTS];
static int s_count;
static int s_next_slot;
static int64_t s_last_record_us;

void sensor_history_init(void) {
  s_count = 0;
  s_next_slot = 0;
  s_last_record_us = -RECORD_INTERVAL_US;  // erzwingt einen Eintrag beim ersten Aufruf
}

void sensor_history_maybe_record(bool ntc_valid, float ntc_temp_c, bool dht_valid, float dht_temp_c,
                                  float dht_humidity_pct) {
  int64_t now = esp_timer_get_time();
  if (now - s_last_record_us < RECORD_INTERVAL_US) return;
  s_last_record_us = now;

  sensor_history_entry_t* e = &s_history[s_next_slot];
  e->ntc_valid = ntc_valid;
  e->ntc_temp_c = ntc_temp_c;
  e->dht_valid = dht_valid;
  e->dht_temp_c = dht_temp_c;
  e->dht_humidity_pct = dht_humidity_pct;
  e->recorded_at_us = now;

  s_next_slot = (s_next_slot + 1) % HISTORY_SLOTS;
  if (s_count < HISTORY_SLOTS) s_count++;
}

size_t sensor_history_get_csv(char* out, size_t out_len) {
  size_t off = snprintf(out, out_len, "uptime_s,ntc_temp_c,dht_temp_c,dht_humidity_pct\n");
  int start = (s_count < HISTORY_SLOTS) ? 0 : s_next_slot;

  for (int i = 0; i < s_count && off < out_len; i++) {
    int idx = (start + i) % HISTORY_SLOTS;
    sensor_history_entry_t* e = &s_history[idx];

    char ntc_buf[16] = "";
    if (e->ntc_valid) snprintf(ntc_buf, sizeof(ntc_buf), "%.1f", e->ntc_temp_c);

    char dt_buf[16] = "";
    char dh_buf[16] = "";
    if (e->dht_valid) {
      snprintf(dt_buf, sizeof(dt_buf), "%.1f", e->dht_temp_c);
      snprintf(dh_buf, sizeof(dh_buf), "%.1f", e->dht_humidity_pct);
    }

    off += snprintf(out + off, out_len - off, "%lld,%s,%s,%s\n", (long long)(e->recorded_at_us / 1000000), ntc_buf,
                     dt_buf, dh_buf);
  }
  return off;
}

void sensor_history_reset(void) {
  s_count = 0;
  s_next_slot = 0;
  s_last_record_us = -RECORD_INTERVAL_US;
}

size_t sensor_history_get_entries(sensor_history_entry_t* out, size_t max_entries) {
  int start = (s_count < HISTORY_SLOTS) ? 0 : s_next_slot;
  size_t n = (size_t)s_count < max_entries ? (size_t)s_count : max_entries;
  for (size_t i = 0; i < n; i++) {
    int idx = (start + (int)i) % HISTORY_SLOTS;
    out[i] = s_history[idx];
  }
  return n;
}
