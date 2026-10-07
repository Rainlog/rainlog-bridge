#include "firmware_image.h"
#include <string.h>

static uint32_t little32(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

const char *firmware_image_validate(const uint8_t *p, size_t prefix_size,
                                   size_t image_size, size_t slot_size,
                                   const char *board, uint16_t chip,
                                   uint8_t flash_size, char version[32]) {
  if (prefix_size < FIRMWARE_PREFIX_SIZE || image_size < FIRMWARE_PREFIX_SIZE)
    return "Firmware file is too short";
  if (image_size > slot_size) return "Firmware does not fit the update slot";
  if (p[0] != 0xe9 || !p[1] || p[1] > 16 ||
      little32(p + 32) != 0xabcd5432 ||
      memcmp(p + 80, "rainlog-wireless-bridge", sizeof("rainlog-wireless-bridge")))
    return "Select a Rainlog Bridge app image, not a merged or bootloader image";
  if ((p[12] | ((uint16_t)p[13] << 8)) != chip || (p[3] >> 4) != flash_size)
    return "Firmware is for a different chip or flash size";
  if (p[23] != 1) return "Firmware must include its SHA-256 integrity hash";
  if (little32(p + 28) < 256 + sizeof(firmware_identity_t) ||
      memcmp(p + FIRMWARE_ID_OFFSET, FIRMWARE_ID_MAGIC, 8) ||
      !memchr(p + FIRMWARE_ID_OFFSET + 8, 0, 40))
    return "Firmware lacks board identity; rebuild it with the current project";
  if (strcmp((const char *)p + FIRMWARE_ID_OFFSET + 8, board))
    return "Firmware is for a different board";
  if (!p[48] || !memchr(p + 48, 0, 32)) return "Invalid firmware version";
  for (const uint8_t *v = p + 48; *v; v++) {
    if (!((*v >= '0' && *v <= '9') || (*v >= 'A' && *v <= 'Z') ||
          (*v >= 'a' && *v <= 'z') || strchr("._+-", *v)))
      return "Invalid firmware version";
  }
  memcpy(version, p + 48, 32);
  return NULL;
}
