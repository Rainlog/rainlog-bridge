#include "rain_upload.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t counter_modulus(const weather_packet_t *p) {
  return p->model == WEATHER_LACROSSE_TX5U ? 4096 :
         p->model == WEATHER_ACURITE_5N1 ? 16384 : 0;
}
static uint32_t tip_microin(const weather_packet_t *p) {
  return p->model == WEATHER_LACROSSE_TX5U ? 10500 : 10000;
}

bool rain_counter_update(rain_counter_t *c, uint32_t gauge,
                         const weather_packet_t *p) {
  uint32_t modulus = counter_modulus(p);
  if (!gauge || !p->has_rain || !modulus || p->rain_raw >= modulus) return false;
  if (c->version != 1 || c->gauge_id != gauge) {
    *c = (rain_counter_t){.version = 1, .gauge_id = gauge,
                         .sensor_id = p->id, .model = p->model,
                         .channel = p->channel, .raw = p->rain_raw,
                         .total_microin = (uint64_t)p->rain_raw * tip_microin(p)};
    return true;
  }
  bool same_sensor = c->sensor_id == p->id && c->model == p->model &&
                     c->channel == p->channel;
  uint32_t delta = 0;
  if (same_sensor) {
    if (p->rain_raw >= c->raw) delta = p->rain_raw - c->raw;
    // A decrease across the ends of the range is rollover. Other decreases
    // are resets. A reset retaining the ID near rollover is indistinguishable.
    else if (c->raw >= modulus * 3 / 4 && p->rain_raw < modulus / 4)
      delta = modulus - c->raw + p->rain_raw;
  }
  bool changed = !same_sensor || c->raw != p->rain_raw;
  c->total_microin += (uint64_t)delta * tip_microin(p);
  c->sensor_id = p->id;
  c->model = p->model;
  c->channel = p->channel;
  c->raw = p->rain_raw;
  return changed;
}

static bool append(char *out, size_t size, size_t *used, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(out + *used, size - *used, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= size - *used) return false;
  *used += n;
  return true;
}

bool rain_upload_encode(char *out, size_t size, uint32_t gauge,
                        const char *key, const weather_packet_t *p,
                        uint64_t total_microin, time_t observed) {
  bool iris = p->model == WEATHER_ACURITE_5N1;
  if (!size || !key || (gauge && !*key) ||
      (!p->has_rain && !(iris && (p->has_wind || p->has_temperature || p->has_wind_direction))) ||
      !counter_modulus(p) || observed < 1600000000) return false;
  struct tm utc;
  if (!gmtime_r(&observed, &utc)) return false;
  size_t used = 0;
  if (gauge) {
    if (!append(out, size, &used, "ID=Rainlog%lu&PASSWORD=", (unsigned long)gauge)) return false;
  } else if (!append(out, size, &used, "ID=&PASSWORD=")) return false;
  for (const unsigned char *s = (const unsigned char *)key; *s; s++) {
    bool unreserved = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                      (*s >= '0' && *s <= '9') || strchr("-_.~", *s);
    if (!append(out, size, &used, unreserved ? "%c" : "%%%02X", *s))
      return false;
  }
  if (!append(out, size, &used,
      "&dateutc=%04d-%02d-%02d%%20%02d%%3A%02d%%3A%02d"
      "&action=updateraw",
      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
      utc.tm_hour, utc.tm_min, utc.tm_sec)) return false;
  if (p->has_rain && !append(out, size, &used, "&totalrainin=%llu.%06llu",
      (unsigned long long)(total_microin / 1000000),
      (unsigned long long)(total_microin % 1000000))) return false;
  if (!append(out, size, &used,
      "&softwaretype=RainlogBridgeRadio&rlsource=radio&sensor_model=%s&sensor_id=%u",
      p->model == WEATHER_LACROSSE_TX5U ? "LaCrosse-TX5U" : "AcuRite-Iris-5n1", p->id))
    return false;
  if (p->model == WEATHER_LACROSSE_TX5U &&
      !append(out, size, &used, "&sensor_channel=")) return false;
  if (p->model == WEATHER_ACURITE_5N1) {
    if (!append(out, size, &used, "&sensor_channel=%c&mt=5N1", p->channel)) return false;
    if (p->has_wind && !append(out, size, &used, "&windspeedmph=%.2f",
                               p->wind_kph / 1.609344)) return false;
    if (p->has_battery && !append(out, size, &used, "&sensorbattery=%s",
                                 p->battery_ok ? "normal" : "low")) return false;
    if (p->has_wind_direction &&
        !append(out, size, &used, "&winddir=%.0f", p->wind_direction)) return false;
    if (p->has_temperature &&
        !append(out, size, &used, "&tempf=%.2f&humidity=%.0f",
                p->temperature_c * 1.8 + 32, p->humidity)) return false;
    // Magnus dew point over water, calculated only from this packet's pair.
    // Zero relative humidity has no finite dew point.
    if (p->has_temperature && p->humidity > 0 && p->humidity <= 100) {
      double gamma = log(p->humidity / 100.0) +
                     17.62 * p->temperature_c / (243.12 + p->temperature_c);
      double dewpoint_f = 243.12 * gamma / (17.62 - gamma) * 1.8 + 32;
      if (!append(out, size, &used, "&dewptf=%.0f", dewpoint_f)) return false;
    }
  }
  return true;
}
