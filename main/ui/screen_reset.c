#include "screen_reset.h"

#include <stdio.h>

#include "display.h"
#include "ui_common.h"

void screen_reset_draw(uint32_t held_ms) {
  ui_center_text(20, 3, COLOR_RED, "FACTORY");
  ui_center_text(60, 3, COLOR_RED, "RESET");

  // Whole seconds left until the reset fires (floor), clamped at 0. With a ~10s
  // hold this starts at 9 and ticks 9, 8, ... 0.
  uint32_t remaining =
      (held_ms >= RESET_HOLD_MS) ? 0 : (RESET_HOLD_MS - held_ms) / 1000;
  char n[8];
  snprintf(n, sizeof(n), "%lu", (unsigned long)remaining);
  ui_center_text(108, 6, COLOR_WHITE, n);  // big countdown digit (78px)

  ui_center_text(208, 1, COLOR_GREY, "Keep holding to");
  ui_center_text(224, 1, COLOR_GREY, "erase all settings");
  ui_center_text(256, 1, COLOR_AMBER, "Release to cancel");
}
