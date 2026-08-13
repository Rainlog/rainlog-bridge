// Rainlog Wireless Bridge - shared UI helpers used by every screen.
#pragma once

#include <stdint.h>

// Top-of-screen header band height anchor (logo pixmap + version line).
#define Y_HEADER 8

// Row where each screen's content starts, with a margin below the version
// line in the header (which ends ~75 with the 8x13 font). Both screens draw
// their content from here for a consistent layout.
#define Y_CONTENT_TOP 82

// Draw a string horizontally centered at row y.
void ui_center_text(int y, int scale, uint16_t color, const char *s);

// Draw the common header: the raindrop + "Rainlog / Bridge" logo pixmap and
// the firmware version line.
void ui_draw_header(void);
