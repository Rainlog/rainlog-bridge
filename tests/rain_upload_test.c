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
  assert(strstr(query, "windspeedmph=10.00&winddir=90"));
  p.message_type = 56; p.has_temperature = true;
  p.temperature_c = 20; p.humidity = 47;
  assert(rain_upload_encode(query, sizeof(query), 54321, "key", &p,
                             state.total_microin, 1700000000));
  assert(strstr(query, "tempf=68.00&humidity=47") && !strstr(query, "winddir="));
  p.model = 42;
  assert(!rain_upload_encode(query, sizeof(query), 54321, "key", &p, 1, 1700000000));
  puts("Rainfall encoding, credentials, timestamps, identity, reset/replacement and rollover passed");
}
