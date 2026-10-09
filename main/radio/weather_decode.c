// Independent implementation of the documented TX5U and AcuRite Iris formats.
// Protocol references and the original TX5U captures are listed in README.md.
#include "weather_decode.h"

#include <string.h>

void weather_stamp_iris(weather_packet_t *p, int64_t received_us) {
  if (p->model != WEATHER_ACURITE_5N1) return;
  p->has_wind = p->has_battery = true;
  p->wind_received_us = received_us;
  if (p->has_rain) p->rain_received_us = received_us;
  if (p->has_temperature) p->temperature_received_us = received_us;
  if (p->has_wind_direction) p->wind_direction_received_us = received_us;
}

void weather_expire_iris(weather_packet_t *p, int64_t now_us) {
  if (p->model != WEATHER_ACURITE_5N1) return;
  if (now_us - p->rain_received_us >= IRIS_FIELD_FRESHNESS_US) p->has_rain = false;
  if (now_us - p->temperature_received_us >= IRIS_FIELD_FRESHNESS_US)
    p->has_temperature = false;
  if (now_us - p->wind_direction_received_us >= IRIS_FIELD_FRESHNESS_US)
    p->has_wind_direction = false;
  if (now_us - p->wind_received_us >= IRIS_FIELD_FRESHNESS_US)
    p->has_wind = p->has_battery = false;
}

void weather_merge_iris(weather_packet_t *current, const weather_packet_t *previous) {
  if (current->model != WEATHER_ACURITE_5N1 ||
      previous->model != current->model || previous->id != current->id ||
      previous->channel != current->channel) return;
  if (!current->has_rain && previous->has_rain) {
    current->has_rain = true;
    current->rain_raw = previous->rain_raw;
    current->rain_mm = previous->rain_mm;
    current->rain_received_us = previous->rain_received_us;
  }
  if (!current->has_temperature && previous->has_temperature) {
    current->has_temperature = true;
    current->temperature_c = previous->temperature_c;
    current->humidity = previous->humidity;
    current->temperature_received_us = previous->temperature_received_us;
  }
  if (!current->has_wind_direction && previous->has_wind_direction) {
    current->has_wind_direction = true;
    current->wind_direction = previous->wind_direction;
    current->wind_direction_received_us = previous->wind_direction_received_us;
  }
}
#if WEATHER_PROTOCOL_OOK
static unsigned parity(unsigned value) {
  unsigned result = 0;
  while (value) {
    result ^= value & 1;
    value >>= 1;
  }
  return result;
}
#endif
#if WEATHER_PROTOCOL_LACROSSE_TX5U
bool weather_decode_lacrosse(const uint8_t bytes[6], weather_packet_t *out) {
  uint8_t n[11];
  unsigned sum = 0;
  for (unsigned i = 0; i < 11; i++)
    n[i] = (bytes[i / 2] >> (i % 2 ? 0 : 4)) & 15;
  for (unsigned i = 0; i < 10; i++) sum += n[i];
  unsigned value = (n[5] << 8) | (n[6] << 4) | n[7];
  if (n[0] != 0 || n[1] != 10 || n[2] != 10 || (sum & 15) != n[10] ||
      parity(value) != (n[4] & 1) || n[5] != n[8] || n[6] != n[9])
    return false;
  *out = (weather_packet_t){.model = WEATHER_LACROSSE_TX5U,
                            .id = (n[3] << 3) | (n[4] >> 1),
                            .message_type = 10,
                            .has_rain = true,
                            .rain_raw = value,
                            .rain_mm = value * 0.2667f};
  memcpy(out->raw, bytes, 6);
  return true;
}
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
bool weather_decode_acurite(const uint8_t bytes[8], weather_packet_t *out) {
  unsigned sum = 0, parity_bit = 0;
  for (unsigned i = 0; i < 7; i++) sum += bytes[i];
  for (unsigned i = 2; i < 7; i++) parity_bit ^= parity(bytes[i]);
  unsigned type = bytes[2] & 63, channel = bytes[0] >> 6;
  if ((sum & 255) != bytes[7] || parity_bit || channel == 1 ||
      (type != 49 && type != 56))
    return false;
  static const unsigned directions[] = {14, 11, 13, 12, 15, 10, 0, 9,
                                        3,  6,  4,  5,  2,  7,  1, 8};
  unsigned wind = ((bytes[3] & 31) << 3) | ((bytes[4] & 112) >> 4);
  *out = (weather_packet_t){.model = WEATHER_ACURITE_5N1,
                            .id = ((bytes[0] & 15) << 8) | bytes[1],
                            .channel = channel == 0   ? 'C'
                                       : channel == 2 ? 'B'
                                                      : 'A',
                            .message_type = type,
                            .sequence = (bytes[0] >> 4) & 3,
                            .battery_ok = !!(bytes[2] & 64),
                            .wind_kph = wind ? wind * 0.8278f + 1.0f : 0};
  if (type == 49) {
    out->has_rain = true;
    out->has_wind_direction = true;
    out->rain_raw = ((bytes[5] & 127) << 7) | (bytes[6] & 127);
    out->rain_mm = out->rain_raw * 0.254f;
    out->wind_direction = directions[bytes[4] & 15] * 22.5f;
  } else {
    int temperature = ((bytes[4] & 15) << 7) | (bytes[5] & 127);
    out->has_temperature = true;
    out->temperature_c = ((temperature - 400) * 0.1f - 32) * 5 / 9;
    out->humidity = bytes[6] & 127;
    if (temperature < 0 || temperature > 1980 || out->humidity > 100)
      return false;
  }
  memcpy(out->raw, bytes, 8);
  return true;
}
#endif
#if WEATHER_PROTOCOL_OOK
static void bytes_from_bits(uint64_t bits, uint8_t *bytes, unsigned count) {
  for (unsigned i = 0; i < count; i++) bytes[i] = bits >> ((count - 1 - i) * 8);
}
#endif
bool weather_decode_duration(weather_decoder_t *d, bool high, uint32_t us,
                             weather_packet_t *out) {
#if WEATHER_PROTOCOL_OOK
  if (!high) {
#if WEATHER_PROTOCOL_LACROSSE_TX5U
    if (us < 600 || us > 1300) d->lacrosse_count = 0;
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
    if (us > 500) d->acurite_count = 0;
#endif
    return false;
  }
#if WEATHER_PROTOCOL_LACROSSE_TX5U
  bool short_lacrosse = us >= 320 && us <= 740;
  bool long_lacrosse = us >= 950 && us <= 1500;
  if (short_lacrosse || long_lacrosse) {
    d->lacrosse_bits =
        ((d->lacrosse_bits << 1) | short_lacrosse) & 0xfffffffffffULL;
    if (d->lacrosse_count < 44) d->lacrosse_count++;
    if (d->lacrosse_count == 44) {
      uint8_t bytes[6];
      bytes_from_bits(d->lacrosse_bits << 4, bytes, 6);
      if (weather_decode_lacrosse(bytes, out)) {
        d->lacrosse_count = 0;
        return true;
      }
    }
  } else
    d->lacrosse_count = 0;
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
  bool short_acurite = us >= 140 && us <= 300;
  bool long_acurite = us >= 330 && us <= 510;
  if (short_acurite || long_acurite) {
    d->acurite_bits = (d->acurite_bits << 1) | long_acurite;
    if (d->acurite_count < 64) d->acurite_count++;
    if (d->acurite_count == 64) {
      uint8_t bytes[8];
      bytes_from_bits(d->acurite_bits, bytes, 8);
      if (weather_decode_acurite(bytes, out)) {
        d->acurite_count = 0;
        return true;
      }
    }
  } else
    d->acurite_count = 0;
#endif
#else
  (void)d;
  (void)high;
  (void)us;
  (void)out;
#endif
  return false;
}
