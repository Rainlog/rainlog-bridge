// Rainlog Wireless Bridge - BOOT button (the only user input on this non-touch
// board). A press pokes the shared activity signal (see activity.h) so the UI
// brightens the backlight; a sustained hold drives the factory-reset countdown.
#pragma once

#include <stdint.h>

void button_init(void);

// Milliseconds the button has been held continuously, or 0 if not held.
uint32_t button_held_ms(void);
