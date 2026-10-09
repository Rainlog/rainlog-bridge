#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "weather_decode.h"

// Persisted native counter and expanded total in millionths of an inch.
typedef struct {
  uint32_t version, gauge_id, sensor_id;
  uint8_t model;
  char channel;
  uint16_t raw;
  uint64_t total_microin;
} rain_counter_t;
// Replacements and counter resets rebase without adding their existing rain.
bool rain_counter_update(rain_counter_t *, uint32_t gauge,
                         const weather_packet_t *);
// Timestamped WU-compatible query, optionally including Rainlog credentials.
// A zero gauge emits no Rainlog ID and is sent only to a device WU uploader.
// Iris may omit expired rain while uploading other fresh fields.
// Returns false on no usable fields, invalid time/model, or insufficient space.
bool rain_upload_encode(char *out, size_t size, uint32_t gauge,
                        const char *key, const weather_packet_t *,
                        uint64_t total_microin, time_t observed);
