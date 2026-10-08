#include "radio_upload.h"
#include "radio.h"
#include "rain_upload.h"
#include "config_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static bridge_config_t config;
static radio_sensor_t sensor;
static rain_counter_t saved, saved_wu;
static int64_t now = 1000000;
static int sends, writes;
static bool receiving = true, fail_storage, full_queue;
static char latest[768];
const bridge_config_t *config_get(void) { return &config; }
const radio_mapping_t *config_find_radio_mapping(uint8_t model, uint32_t id, char channel) {
  for (unsigned i = 0; i < config.radio_map_count; i++) {
    radio_mapping_t *map = &config.radio_map[i];
    if (map->gauge_id && map->model == model && map->sensor_id == id && map->channel == channel) return map;
  }
  return NULL;
}
int64_t esp_timer_get_time(void) { return now; }
time_t time(time_t *out) {
  time_t wall = 1700000000 + now / 1000000;
  if (out) *out = wall;
  return wall;
}
void radio_status(radio_status_t *out) { *out = (radio_status_t){.receiving = receiving}; }
size_t radio_sensors(radio_sensor_t *out, size_t count) {
  assert(count); *out = sensor; return 1;
}
int nvs_open(const char *name, int mode, int *handle) {
  assert(!strcmp(name, "radio_rain") && mode == NVS_READWRITE);
  *handle = 1; return fail_storage ? 1 : 0;
}
void nvs_close(int handle) { (void)handle; }
int nvs_get_blob(int handle, const char *key, void *out, size_t *size) {
  (void)handle; assert(*size == sizeof(saved));
  rain_counter_t *counter = !strcmp(key, "g12345") ? &saved : &saved_wu;
  if (!counter->gauge_id) return ESP_ERR_NVS_NOT_FOUND;
  memcpy(out, counter, *size); return 0;
}
int nvs_set_blob(int handle, const char *key, const void *value, size_t size) {
  (void)handle; assert(size == sizeof(saved));
  rain_counter_t *counter = !strcmp(key, "g12345") ? &saved : &saved_wu;
  assert(!strcmp(key, "g12345") || !strcmp(key, "r0_7_0"));
  memcpy(counter, value, size); writes++; return 0;
}
int nvs_commit(int handle) { (void)handle; return 0; }
static bool send_query(const char *query) {
  if (full_queue) return false;
  sends++; snprintf(latest, sizeof(latest), "%s", query);
  // Persistence precedes transmission.
  assert(saved.gauge_id == 12345);
  return true;
}
int main(void) {
  config.radio_enabled = 1;
  sensor.reading = (radio_reading_t){.packet = {.model = WEATHER_LACROSSE_TX5U,
      .id = 7, .has_rain = true, .rain_raw = 3}, .received_us = now};
  radio_upload_poll(send_query);
  assert(!sends && !writes); // Unmapped sensors do not transmit.
  config.radio_map_count = 1;
  config.radio_map[0] = (radio_mapping_t){.sensor_id = 7, .gauge_id = 12345,
                                         .rainlog_key = "test-key"};
  fail_storage = true;
  radio_upload_poll(send_query);
  assert(!sends);
  fail_storage = false;
  full_queue = true;
  radio_upload_poll(send_query);
  assert(sends == 0);
  full_queue = false;
  radio_upload_poll(send_query);
  assert(sends == 1 && writes == 1 && strstr(latest, "totalrainin=0.031500"));
  radio_upload_poll(send_query);
  assert(sends == 1 && writes == 1); // No replay of one snapshot.
  now += 20000000; sensor.reading.received_us = now;
  sensor.reading.packet.rain_raw++;
  radio_upload_poll(send_query);
  assert(sends == 1 && writes == 2 && saved.total_microin == 42000);
  now += 300000000; sensor.reading.received_us = now;
  radio_upload_poll(send_query);
  assert(sends == 2 && strstr(latest, "totalrainin=0.042000"));
  char previous_query[sizeof(latest)];
  snprintf(previous_query, sizeof(previous_query), "%s", latest);
  now += 299999999;
  radio_upload_poll(send_query);
  assert(sends == 2); // No repeat before 300 seconds.
  now++;
  radio_upload_poll(send_query);
  assert(sends == 3 && strstr(latest, "totalrainin=0.042000")); // Cached snapshot at exactly 300s.
  assert(strcmp(previous_query, latest)); // The repeated total has a new snapshot timestamp.
  sensor.reading.received_us = now;
  radio_upload_poll(send_query);
  assert(sends == 3); // A fresh duplicate does not bypass the interval.
  receiving = false; now += 310000000; sensor.reading.received_us = now;
  radio_upload_poll(send_query);
  assert(sends == 3);
  receiving = true;
  fail_storage = true; sensor.reading.packet.rain_raw++;
  radio_upload_poll(send_query);
  assert(sends == 3); // Cannot publish a non-persisted counter.
  fail_storage = false;
  radio_upload_poll(send_query);
  assert(sends == 4 && saved.total_microin == 52500);
  // Switching away and back reloads the same persisted baseline.
  config.radio_map[0].gauge_id = 0;
  radio_upload_poll(send_query);
  config.radio_map[0].gauge_id = 12345;
  sensor.reading.packet.rain_raw++;
  now += 310000000; sensor.reading.received_us = now;
  radio_upload_poll(send_query);
  assert(saved.total_microin == 63000 && strstr(latest, "totalrainin=0.063000"));
  config.radio_map_count = 0;
  config.wu_map_count = 1;
  strcpy(config.wu_map[0].device, "radio:0:7:0");
  strcpy(config.wu_map[0].wu_id, "WUONLY");
  strcpy(config.wu_map[0].wu_key, "wu-key");
  now += 310000000; sensor.reading.received_us = now;
  int previous = sends;
  radio_upload_poll(send_query);
  assert(sends == previous + 1 && saved_wu.version == 1);
  assert(strstr(latest, "ID=&PASSWORD=&dateutc=") && !strstr(latest, "ID=Rainlog"));
  radio_upload_poll(send_query);
  assert(sends == previous + 1);
  config.radio_map_count = 1;
  now += 310000000; sensor.reading.received_us = now;
  radio_upload_poll(send_query);
  assert(sends == previous + 2); // Two uploaders share one sensor submission.
  puts("Radio uploader mapping, cadence, periodic snapshots, storage failure and recovery passed");
}
