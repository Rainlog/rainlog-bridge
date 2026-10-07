// Board identity at the fixed ESP-IDF custom app-description offset.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_ID_MAGIC "RLOGOTA1"
#define FIRMWARE_ID_OFFSET (24 + 8 + 256)
#define FIRMWARE_PREFIX_SIZE (FIRMWARE_ID_OFFSET + 48)
typedef struct {
  char magic[8];
  char board[40];
} firmware_identity_t;

// Validate the initial image bytes before touching an OTA partition.
// Returns a user-facing error or NULL. version receives a terminated string.
const char *firmware_image_validate(const uint8_t *prefix, size_t prefix_size,
                                   size_t image_size, size_t slot_size,
                                   const char *board, uint16_t chip,
                                   uint8_t flash_size, char version[32]);
