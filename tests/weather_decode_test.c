#include "weather_decode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void acurite_frame(uint8_t frame[8], unsigned type, unsigned raw) {
  const uint8_t base[8] = {0x0c, 0xc7, 0, 0, 0, 0, 0, 0};
  memcpy(frame, base, 8);
  frame[2] = type;
  frame[5] = raw >> 7;
  frame[6] = raw & 127;
  for (unsigned i = 2; i < 7; i++) {
    unsigned parity = 0;
    for (unsigned bit = 0; bit < 7; bit++) parity ^= (frame[i] >> bit) & 1;
    frame[i] |= parity << 7;
  }
  unsigned sum = 0;
  for (unsigned i = 0; i < 7; i++) sum += frame[i];
  frame[7] = sum;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *file = fopen(argv[1], "r");
  assert(file);
  weather_decoder_t decoder = {0};
  weather_packet_t packet;
  unsigned seen[4096] = {0};
  char line[128];
  while (fgets(line, sizeof(line), file)) {
    unsigned high, duration;
    if (sscanf(line, "%u %u", &high, &duration) != 2) {
      decoder = (weather_decoder_t){0};
      continue;
    }
    if (weather_decode_duration(&decoder, high, duration, &packet)) {
      assert(packet.model == WEATHER_LACROSSE_TX5U && packet.id == 4 &&
             packet.has_rain);
      seen[packet.rain_raw]++;
    }
  }
  fclose(file);
  assert(seen[14] >= 2 && seen[15] >= 2 && seen[25] >= 2);
  for (unsigned tip = 18; tip <= 21; tip++) assert(seen[tip]);
  uint8_t lacrosse[] = {0x0a, 0xa0, 0x90, 0x0e, 0x00, 0xb0};
  assert(weather_decode_lacrosse(lacrosse, &packet) && packet.rain_raw == 14);
  for (unsigned bit = 0; bit < 44; bit++) {
    unsigned byte = bit / 8, mask = 1u << (7 - bit % 8);
    lacrosse[byte] ^= mask;
    assert(!weather_decode_lacrosse(lacrosse, &packet));
    lacrosse[byte] ^= mask;
  }
  uint8_t frame[8];
  acurite_frame(frame, 49, 61);
  assert(weather_decode_acurite(frame, &packet) && packet.id == 3271 &&
         packet.channel == 'C' && packet.rain_raw == 61);
  decoder = (weather_decoder_t){0};
  unsigned events = 0;
  for (unsigned bit = 0; bit < 64; bit++) {
    bool one = (frame[bit / 8] >> (7 - bit % 8)) & 1;
    if (weather_decode_duration(&decoder, true, one ? 408 : 220, &packet))
      events++;
    weather_decode_duration(&decoder, false, one ? 204 : 392, &packet);
  }
  assert(events == 1 && packet.rain_raw == 61);
  for (unsigned bit = 0; bit < 64; bit++) {
    unsigned byte = bit / 8, mask = 1u << (7 - bit % 8);
    frame[byte] ^= mask;
    assert(!weather_decode_acurite(frame, &packet));
    frame[byte] ^= mask;
  }
  acurite_frame(frame, 56, 47);
  frame[4] = 8;
  frame[5] = 96;
  for (unsigned i = 2; i < 7; i++) {
    frame[i] &= 127;
    unsigned parity = 0;
    for (unsigned bit = 0; bit < 7; bit++) parity ^= (frame[i] >> bit) & 1;
    frame[i] |= parity << 7;
  }
  unsigned sum = 0;
  for (unsigned i = 0; i < 7; i++) sum += frame[i];
  frame[7] = sum;
  assert(weather_decode_acurite(frame, &packet) && packet.has_temperature &&
         packet.humidity == 47);
  puts(
      "Recorded TX5U captures, AcuRite pulse frames and corruption checks "
      "passed");
}
