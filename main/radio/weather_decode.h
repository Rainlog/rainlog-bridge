#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "weather_protocols.h"
typedef enum { WEATHER_LACROSSE_TX5U, WEATHER_ACURITE_5N1 } weather_model_t;
typedef struct {
  weather_model_t model;
  uint16_t id, rain_raw;
  uint8_t message_type, sequence;
  char channel;
  bool battery_ok, has_rain, has_temperature, has_wind_direction;
  bool has_wind, has_battery;
  int64_t rain_received_us, temperature_received_us, wind_direction_received_us;
  int64_t wind_received_us;
  float rain_mm, temperature_c, humidity, wind_kph, wind_direction;
  uint8_t raw[8];
} weather_packet_t;
typedef struct {
#if WEATHER_PROTOCOL_LACROSSE_TX5U
  uint64_t lacrosse_bits;
  unsigned lacrosse_count;
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
  uint64_t acurite_bits;
  unsigned acurite_count;
#endif
#if !WEATHER_PROTOCOL_OOK
  unsigned char unused;
#endif
} weather_decoder_t;
// Complete an Iris snapshot from this sensor's previously received fields.
// Latest frame metadata and fields it supplies always take precedence.
void weather_merge_iris(weather_packet_t *, const weather_packet_t *previous);
void weather_stamp_iris(weather_packet_t *, int64_t received_us);
// User-requested five-minute freshness window for Iris upload fields.
#define IRIS_FIELD_FRESHNESS_US ((int64_t)300 * 1000000)
void weather_expire_iris(weather_packet_t *, int64_t now_us);
// Feed each high or low OOK duration in microseconds. Returns a verified frame.
bool weather_decode_duration(weather_decoder_t *, bool high, uint32_t us,
                             weather_packet_t *);
#if WEATHER_PROTOCOL_LACROSSE_TX5U
bool weather_decode_lacrosse(const uint8_t bytes[6], weather_packet_t *);
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
bool weather_decode_acurite(const uint8_t bytes[8], weather_packet_t *);
#endif
