#include "radio_upload.h"
#include "config_store.h"
#if RAINLOG_RADIO
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "radio.h"
#include "rain_upload.h"

static const char *TAG = "radio-upload";
// Rainlog's one-reading-per-300-second limit, with the forwarder's 5s margin.
#define UPLOAD_INTERVAL_US ((int64_t)305 * 1000000)
static struct {
  uint32_t gauge;
  rain_counter_t counter;
  int64_t last_upload, last_reading;
  bool dirty;
} states[RADIO_MAP_MAX];

static bool load_counter(uint32_t gauge, rain_counter_t *counter) {
  nvs_handle_t nvs;
  if (nvs_open("radio_rain", NVS_READWRITE, &nvs) != ESP_OK) return false;
  char key[16];
  snprintf(key, sizeof(key), "g%lu", (unsigned long)gauge);
  size_t size = sizeof(*counter);
  esp_err_t err = nvs_get_blob(nvs, key, counter, &size);
  nvs_close(nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    memset(counter, 0, sizeof(*counter));
    return true;
  }
  return err == ESP_OK && size == sizeof(*counter) &&
         counter->version == 1 && counter->gauge_id == gauge;
}
static bool save_counter(const rain_counter_t *counter) {
  nvs_handle_t nvs;
  if (nvs_open("radio_rain", NVS_READWRITE, &nvs) != ESP_OK) return false;
  char key[16];
  snprintf(key, sizeof(key), "g%lu", (unsigned long)counter->gauge_id);
  esp_err_t err = nvs_set_blob(nvs, key, counter, sizeof(*counter));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err == ESP_OK;
}

void radio_upload_poll(bool (*submit)(const char *)) {
  radio_status_t radio;
  radio_status(&radio);
  if (!radio.receiving || !config_get()->radio_enabled) return;
  radio_sensor_t sensors[RADIO_SENSORS_MAX];
  size_t count = radio_sensors(sensors, RADIO_SENSORS_MAX);
  int64_t now = esp_timer_get_time();
  time_t wall = time(NULL);
  for (unsigned i = 0; i < config_get()->radio_map_count; i++) {
    // Copy because settings can be saved while a network upload is running.
    radio_mapping_t map = config_get()->radio_map[i];
    if (states[i].gauge != map.gauge_id) {
      rain_counter_t counter = {0};
      if (!load_counter(map.gauge_id, &counter)) {
        ESP_LOGW(TAG, "Cannot load counter for gauge %lu", (unsigned long)map.gauge_id);
        continue;
      }
      memset(&states[i], 0, sizeof(states[i]));
      states[i].gauge = map.gauge_id;
      states[i].counter = counter;
    }
    const radio_reading_t *reading = NULL;
    for (size_t j = 0; j < count; j++) {
      const weather_packet_t *p = &sensors[j].reading.packet;
      if (p->model == map.model && p->id == map.sensor_id && p->channel == map.channel)
        reading = &sensors[j].reading;
    }
    if (!reading || !reading->packet.has_rain) continue;
    if (rain_counter_update(&states[i].counter, map.gauge_id, &reading->packet))
      states[i].dirty = true;
    if (states[i].dirty) {
      if (!save_counter(&states[i].counter)) {
        ESP_LOGW(TAG, "Cannot persist counter for gauge %lu", (unsigned long)map.gauge_id);
        continue;
      }
      states[i].dirty = false;
    }
    // Repeated frames do not trigger repeated uploads. Unchanged counters still
    // send periodic live snapshots, giving Rainlog a dry-weather baseline.
    if (reading->received_us == states[i].last_reading ||
        (states[i].last_upload && now - states[i].last_upload < UPLOAD_INTERVAL_US))
      continue;
    time_t observed = wall;
    if (observed < 1600000000) continue; // Wait for SNTP; never backdate with "now".
    observed -= (now - reading->received_us) / 1000000;
    char query[768];
    if (!rain_upload_encode(query, sizeof(query), map.gauge_id, map.rainlog_key,
                            &reading->packet, states[i].counter.total_microin, observed))
      continue;
    if (submit(query)) {
      states[i].last_reading = reading->received_us;
      states[i].last_upload = now;
    }
  }
}
#else
void radio_upload_poll(bool (*submit)(const char *)) { (void)submit; }
#endif
