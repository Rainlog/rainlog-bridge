// Rainlog Wireless Bridge - user-activity signal.
//
// Cross-cutting "someone is interacting" timestamp poked by any input: a BOOT
// button press or a saved setting change (config web page). The UI uses it to
// keep the LCD backlight bright, dimming after an idle window.
#pragma once

#include <stdint.h>

// Initialize to boot time (screen starts bright on power-up).
void activity_init(void);

// Mark "now" as user activity (brightens the screen, resets the idle timer).
void activity_poke(void);

// esp_timer timestamp (us) of the most recent activity.
int64_t activity_last_us(void);
