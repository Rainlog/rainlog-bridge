// SSD1306 horizontal addressing: page-major bytes, eight vertical pixels
// per byte, with the top pixel in bit zero. Preserve every non-black UI color.
#pragma once
#include <stdint.h>

#include "board.h"

static inline void ssd1306_pack_pixels(const uint16_t *rgb, uint8_t *pages) {
  for (int page = 0; page < BOARD_DISPLAY_H / 8; page++) {
    for (int x = 0; x < BOARD_DISPLAY_W; x++) {
      uint8_t bits = 0;
      for (int bit = 0; bit < 8; bit++) {
        if (rgb[(page * 8 + bit) * BOARD_DISPLAY_W + x] != 0)
          bits |= (uint8_t)(1U << bit);
      }
      pages[page * BOARD_DISPLAY_W + x] = bits;
    }
  }
}
