#include "rain_upload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  weather_packet_t p = {.model = WEATHER_LACROSSE_TX5U, .id = 7,
                         .has_rain = true, .rain_raw = 3};
  rain_counter_t state = {0};
  assert(rain_counter_update(&state, 12345, &p));
  assert(state.total_microin == 31500);
  assert(!rain_counter_update(&state, 12345, &p));
  p.rain_raw = 4;
  assert(rain_counter_update(&state, 12345, &p));
  assert(state.total_microin == 42000);
  // Reset: preserve the total and restart the raw baseline.
  p.rain_raw = 0;
  assert(rain_counter_update(&state, 12345, &p));
  assert(state.total_microin == 42000);
  p.rain_raw = 1;
  rain_counter_update(&state, 12345, &p);
  assert(state.total_microin == 52500);
  // Sensor replacement preserves the gauge's total, not the replacement's past tips.
  p.id = 8; p.rain_raw = 100;
  rain_counter_update(&state, 12345, &p);
  assert(state.total_microin == 52500);
  // Simulated persisted state survives a reboot and crosses rollover.
  state.raw = 4095;
  rain_counter_t restored = state;
  p.rain_raw = 1;
  rain_counter_update(&restored, 12345, &p);
  assert(restored.total_microin == 73500);
  char query[768];
  assert(rain_upload_encode(query, sizeof(query), 12345, "a b&=+%", &p,
                             restored.total_microin, 1700000000));
  assert(strstr(query, "ID=Rainlog12345&PASSWORD=a%20b%26%3D%2B%25"));
  assert(strstr(query, "dateutc=2023-11-14%2022%3A13%3A20"));
  assert(strstr(query, "totalrainin=0.073500"));
  assert(strstr(query, "sensor_model=LaCrosse-TX5U&sensor_id=8&sensor_channel="));
  assert(!strstr(query, "rainin=") || strstr(query, "totalrainin="));
  assert(!strstr(query, "tempf=") && !strstr(query, "windspeedmph="));
  assert(!rain_upload_encode(query, 10, 12345, "key", &p, 1, 1700000000));
  assert(!rain_upload_encode(query, sizeof(query), 12345, "key", &p, 1, 0));
  p.has_rain = false;
  assert(!rain_upload_encode(query, sizeof(query), 12345, "key", &p, 1, 1700000000));
  p = (weather_packet_t){.model = WEATHER_ACURITE_5N1, .id = 3271,
                          .channel = 'C', .has_rain = true, .rain_raw = 16383,
                          .message_type = 49, .wind_kph = 16.09344,
                          .has_wind_direction = true,
                          .has_wind = true, .has_battery = true,
                          .wind_direction = 90};
  state = (rain_counter_t){0};
  rain_counter_update(&state, 54321, &p);
  p.rain_raw = 0;
  rain_counter_update(&state, 54321, &p);
  assert(state.total_microin == 163840000);
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &p,
                             state.total_microin, 1700000000));
  assert(strstr(query, "totalrainin=163.840000"));
  assert(strstr(query, "sensor_model=AcuRite-Iris-5n1&sensor_id=3271&sensor_channel=C"));
  assert(strstr(query, "windspeedmph=10.00"));
  assert(strstr(query, "&winddir=90"));
  assert(strstr(query, "&mt=5N1") && strstr(query, "&sensorbattery=low"));
  assert(!strstr(query, "tempf=") && !strstr(query, "humidity=") &&
         !strstr(query, "dewptf="));
  p.message_type = 56; p.has_temperature = true;
  p.has_wind_direction = false;
  p.temperature_c = 20; p.humidity = 47; p.battery_ok = true;
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &p,
                             state.total_microin, 1700000000));
  assert(strstr(query, "tempf=68.00&humidity=47") && !strstr(query, "winddir="));
  assert(strstr(query, "&dewptf=47") && strstr(query, "sensorbattery=normal"));
  assert(!strstr(query, "baromin=") && !strstr(query, "hubbattery=") &&
         !strstr(query, "windgustmph=") && !strstr(query, "&rainin=") &&
         !strstr(query, "dailyrainin="));
  p.humidity = 100;
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &p,
                             state.total_microin, 1700000000));
  assert(strstr(query, "&dewptf=68"));
  p.humidity = 0;
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &p,
                             state.total_microin, 1700000000));
  assert(!strstr(query, "dewptf="));
  // The two Iris frames form one snapshot regardless of arrival order.
  weather_packet_t temperature = p;
  temperature.humidity = 47;
  weather_stamp_iris(&temperature, 1000000);
  weather_packet_t wind = temperature;
  wind.message_type = 49;
  wind.has_temperature = false;
  wind.has_wind_direction = true;
  wind.wind_direction = 90;
  wind.wind_kph = 8.04672;
  wind.battery_ok = false;
  weather_stamp_iris(&wind, 2000000);
  weather_packet_t combined = wind;
  weather_merge_iris(&combined, &temperature);
  assert(combined.message_type == 49 && combined.has_temperature &&
         combined.temperature_c == 20 && combined.humidity == 47 &&
         combined.wind_direction == 90 && !combined.battery_ok);
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &combined,
                             state.total_microin, 1700000000));
  assert(strstr(query, "&winddir=90") && strstr(query, "&tempf=68.00") &&
         strstr(query, "&humidity=47") && strstr(query, "&dewptf=47") &&
         strstr(query, "windspeedmph=5.00") && strstr(query, "sensorbattery=low"));
  // A new temperature frame retains direction/rain, but replaces its own pair.
  temperature.has_rain = false;
  temperature.temperature_c = 25;
  temperature.humidity = 60;
  weather_merge_iris(&temperature, &combined);
  assert(temperature.has_rain && temperature.rain_raw == wind.rain_raw &&
         temperature.has_wind_direction && temperature.wind_direction == 90 &&
         temperature.temperature_c == 25 && temperature.humidity == 60 &&
         temperature.battery_ok);
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &temperature,
                             state.total_microin, 1700000000));
  assert(strstr(query, "&winddir=90") && strstr(query, "&tempf=77.00&humidity=60"));
  // An empty inventory cannot invent direction, including a valid north (0).
  weather_packet_t empty = {0};
  temperature.has_wind_direction = false;
  weather_merge_iris(&temperature, &empty);
  assert(!temperature.has_wind_direction);
  combined.wind_direction = 0;
  weather_merge_iris(&temperature, &combined);
  assert(temperature.has_wind_direction && temperature.wind_direction == 0);
  // Sensors sharing an ID on another channel must remain independent.
  for (unsigned mismatch = 0; mismatch < 3; mismatch++) {
    weather_packet_t other = combined;
    if (mismatch == 0) other.model = WEATHER_LACROSSE_TX5U;
    if (mismatch == 1) other.id++;
    if (mismatch == 2) other.channel = 'A';
    weather_packet_t unmerged = wind;
    weather_merge_iris(&unmerged, &other);
    assert(!unmerged.has_temperature);
  }
  weather_packet_t tx5u = {.model = WEATHER_LACROSSE_TX5U, .id = 7};
  weather_packet_t old_tx5u = tx5u;
  old_tx5u.has_rain = true;
  old_tx5u.rain_raw = 9;
  weather_merge_iris(&tx5u, &old_tx5u);
  assert(!tx5u.has_rain && tx5u.rain_raw == 0);
  // Receiving wind/rain does not refresh the temperature family's clock.
  weather_packet_t fresh = wind;
  weather_stamp_iris(&fresh, IRIS_FIELD_FRESHNESS_US);
  weather_merge_iris(&fresh, &combined);
  weather_packet_t expiring = fresh;
  weather_expire_iris(&expiring, 1000000 + IRIS_FIELD_FRESHNESS_US - 1);
  assert(expiring.has_temperature && expiring.has_wind_direction && expiring.has_rain);
  weather_expire_iris(&expiring, 1000000 + IRIS_FIELD_FRESHNESS_US);
  assert(!expiring.has_temperature && expiring.has_wind_direction && expiring.has_rain);
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &expiring,
                             state.total_microin, 1700000000));
  assert(!strstr(query, "tempf=") && !strstr(query, "humidity=") && !strstr(query, "dewptf="));
  // Conversely, a fresh temperature frame cannot keep direction/rain alive.
  fresh = temperature;
  fresh.has_rain = fresh.has_wind_direction = false;
  weather_stamp_iris(&fresh, IRIS_FIELD_FRESHNESS_US);
  weather_merge_iris(&fresh, &combined);
  weather_expire_iris(&fresh, 2000000 + IRIS_FIELD_FRESHNESS_US);
  assert(fresh.has_temperature && fresh.has_wind && !fresh.has_rain && !fresh.has_wind_direction);
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &fresh,
                             state.total_microin, 1700000000));
  assert(strstr(query, "tempf=") && !strstr(query, "winddir=") && !strstr(query, "totalrainin="));
  weather_expire_iris(&fresh, 2 * IRIS_FIELD_FRESHNESS_US);
  assert(!fresh.has_temperature && !fresh.has_wind && !fresh.has_battery);
  assert(!rain_upload_encode(query, sizeof(query), 54321, "key", &fresh,
                              state.total_microin, 1700000000));
  weather_packet_t lacrosse = old_tx5u;
  weather_expire_iris(&lacrosse, 2 * IRIS_FIELD_FRESHNESS_US);
  assert(lacrosse.has_rain && lacrosse.rain_raw == 9);
  p.model = 42;
  assert(!rain_upload_encode(query, sizeof(query), 54321, "key", &p, 1, 1700000000));
  puts("Rainfall encoding, credentials, timestamps, identity, reset/replacement and rollover passed");
}
