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
// Rainlog requires periodic counter snapshots, including unchanged totals.
#define UPLOAD_INTERVAL_US ((int64_t)300 * 1000000)
static struct {
  uint32_t gauge;
  char storage_key[16];
  rain_counter_t counter;
  int64_t last_upload;
  bool dirty;
} states[RADIO_MAP_MAX + WU_MAP_MAX];

static bool load_counter(const char *key, uint32_t gauge, rain_counter_t *counter) {
  nvs_handle_t nvs;
  if (nvs_open("radio_rain", NVS_READWRITE, &nvs) != ESP_OK) return false;
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
static bool save_counter(const char *key, const rain_counter_t *counter) {
  nvs_handle_t nvs;
  if (nvs_open("radio_rain", NVS_READWRITE, &nvs) != ESP_OK) return false;
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
  unsigned rainlog_count = config_get()->radio_map_count;
  for (unsigned i = 0; i < rainlog_count + config_get()->wu_map_count; i++) {
    radio_mapping_t map = {0};
    if (i < rainlog_count) {
      map = config_get()->radio_map[i];
      if (!map.gauge_id) continue;
    }
    else {
      wu_mapping_t wu = config_get()->wu_map[i - rainlog_count];
      unsigned model, id, channel;
      int end = 0;
      if (sscanf(wu.device, "radio:%u:%u:%u%n", &model, &id, &channel, &end) != 3 ||
          wu.device[end] || model > 1 || id > UINT16_MAX || channel > UINT8_MAX) continue;
      // A Rainlog mapping already submits this sensor to both targets.
      if (config_find_radio_mapping(model, id, (char)channel)) continue;
      map.model = model; map.sensor_id = id; map.channel = (char)channel;
    }
    char storage_key[16];
    uint32_t counter_gauge = map.gauge_id ? map.gauge_id : 1;
    if (map.gauge_id) snprintf(storage_key, sizeof(storage_key), "g%lu", (unsigned long)map.gauge_id);
    else snprintf(storage_key, sizeof(storage_key), "r%u_%u_%u", map.model,
                  (unsigned)(uint16_t)map.sensor_id, (unsigned char)map.channel);
    if (strcmp(states[i].storage_key, storage_key)) {
      rain_counter_t counter = {0};
      if (!load_counter(storage_key, counter_gauge, &counter)) {
        ESP_LOGW(TAG, "Cannot load counter %s", storage_key);
        continue;
      }
      memset(&states[i], 0, sizeof(states[i]));
      states[i].gauge = counter_gauge;
      snprintf(states[i].storage_key, sizeof(states[i].storage_key), "%s", storage_key);
      states[i].counter = counter;
    }
    const radio_reading_t *reading = NULL;
    for (size_t j = 0; j < count; j++) {
      const weather_packet_t *p = &sensors[j].reading.packet;
      if (p->model == map.model && p->id == map.sensor_id && p->channel == map.channel)
        reading = &sensors[j].reading;
    }
    if (!reading) continue;
    if (reading->packet.has_rain &&
        rain_counter_update(&states[i].counter, counter_gauge, &reading->packet))
      states[i].dirty = true;
    if (states[i].dirty) {
      if (!save_counter(storage_key, &states[i].counter)) {
        ESP_LOGW(TAG, "Cannot persist counter for gauge %lu", (unsigned long)map.gauge_id);
        continue;
      }
      states[i].dirty = false;
    }
    // TX5U repeats its counter; Iris uploads only fields received within five minutes.
    if (states[i].last_upload && now - states[i].last_upload < UPLOAD_INTERVAL_US)
      continue;
    time_t observed = wall;
    if (observed < 1600000000) continue; // Wait for SNTP; never backdate with "now".
    // This is a current counter snapshot; retries retain this timestamp.
    char query[768];
    weather_packet_t snapshot = reading->packet;
    weather_expire_iris(&snapshot, now);
    if (!rain_upload_encode(query, sizeof(query), map.gauge_id, map.rainlog_key,
                            &snapshot, states[i].counter.total_microin, observed))
      continue;
    if (submit(query)) {
      states[i].last_upload = now;
    }
  }
}
#else
void radio_upload_poll(bool (*submit)(const char *)) { (void)submit; }
#endif
