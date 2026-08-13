// Rainlog Wireless Bridge - factory-reset countdown screen, shown while the
// BOOT button is held. Hold through the countdown to erase settings; release to
// cancel.
#pragma once

#include <stdint.h>

// Hold duration (ms) at which the reset fires. ~11s; the countdown shows whole
// seconds remaining (floor), so it starts at 10 and ticks down to 0.
#define RESET_HOLD_MS 10999
// Hold duration (ms) past which the countdown screen takes over (a short tap is
// just backlight-wake, not a reset).
#define RESET_ARM_MS 600

// Draw the countdown for the current hold duration.
void screen_reset_draw(uint32_t held_ms);
