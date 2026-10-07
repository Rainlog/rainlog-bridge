#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware_image.h"

static const char *board = "lilygo-t3-v1.6.1-sx1278";
static const char *validate(const uint8_t *data, size_t len, size_t size, size_t slot) {
  char version[32];
  return firmware_image_validate(data, len, size, slot, board, 0, 2, version);
}
int main(int argc, char **argv) {
  uint8_t good[FIRMWARE_PREFIX_SIZE] = {0}, bad[FIRMWARE_PREFIX_SIZE];
  good[0] = 0xe9; good[1] = 6; good[3] = 0x20; good[23] = 1;
  good[28] = 0x30; good[29] = 1;  // 304-byte initial segment.
  good[32] = 0x32; good[33] = 0x54; good[34] = 0xcd; good[35] = 0xab;
  memcpy(good + 48, "1.2.3", 6);
  memcpy(good + 80, "rainlog-wireless-bridge", sizeof("rainlog-wireless-bridge"));
  memcpy(good + 288, FIRMWARE_ID_MAGIC, 8);
  strcpy((char *)good + 296, board);
  assert(!validate(good, sizeof(good), 1024, 2048));
  assert(validate(good, sizeof(good) - 1, 1024, 2048));
  assert(validate(good, sizeof(good), 100, 2048));
  assert(validate(good, sizeof(good), 2049, 2048));
  const unsigned corrupt[] = {0, 1, 3, 12, 23, 32, 48, 80, 103, 288, 296};
  for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
    memcpy(bad, good, sizeof(bad));
    bad[corrupt[i]] ^= 0xff;
    assert(validate(bad, sizeof(bad), 1024, 2048));
  }
  memcpy(bad, good, sizeof(bad)); memset(bad + 28, 0, 4);
  assert(validate(bad, sizeof(bad), 1024, 2048));
  memcpy(bad, good, sizeof(bad)); memset(bad + 48, 'v', 32);
  assert(validate(bad, sizeof(bad), 1024, 2048));
  memcpy(bad, good, sizeof(bad)); bad[48] = '"';
  assert(validate(bad, sizeof(bad), 1024, 2048));
  memcpy(bad, good, sizeof(bad)); memset(bad + 296, 'x', 40);
  assert(validate(bad, sizeof(bad), 1024, 2048));
  for (int i = 1; i < argc; i++) {
    FILE *file = fopen(argv[i], "rb"); assert(file);
    assert(fread(bad, 1, sizeof(bad), file) == sizeof(bad));
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); fclose(file);
    const char *error = validate(bad, sizeof(bad), size, 0x180000);
    assert(i == 1 ? error == NULL : error != NULL);
  }
  puts("Firmware identity, bounds, wrong-board and malformed-image checks passed");
}
