// Rainlog Wireless Bridge - LCD display primitives.
//
// Drives the selected ST7789 LCD or SSD1306 OLED via esp_lcd, rendering a
// board-selected bitmap font (X11 misc-fixed) into an in-RAM
// framebuffer. Caller composes a frame with display_clear/display_text, then
// display_flush pushes it to the panel.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#define DISPLAY_W BOARD_DISPLAY_W
#define DISPLAY_H BOARD_DISPLAY_H

// Text at scale s advances GLYPH*s pixels and is GLYPH_H*s pixels tall.
#if BOARD_DISPLAY_FONT == BOARD_FONT_6X10
#define GLYPH 6
#define GLYPH_H 10
#elif BOARD_DISPLAY_FONT == BOARD_FONT_8X13
#define GLYPH 8
#define GLYPH_H 13
#else
#error "Unsupported BOARD_DISPLAY_FONT"
#endif

// OLED colors are on/off; LCD colors are RGB565.
#if BOARD_DISPLAY_SSD1306
typedef uint8_t display_color_t;
#define DISPLAY_FB_BYTES (DISPLAY_W * DISPLAY_H / 8)
#else
typedef uint16_t display_color_t;
#define DISPLAY_FB_BYTES (DISPLAY_W * DISPLAY_H * sizeof(display_color_t))
#endif
display_color_t display_rgb(uint8_t r, uint8_t g, uint8_t b);

// Common colors (lazily-evaluated via display_rgb in callers is fine, but these
// macros keep call sites readable).
#define COLOR_BLACK display_rgb(0, 0, 0)
#define COLOR_WHITE display_rgb(255, 255, 255)
#define COLOR_RED display_rgb(255, 0, 0)
#define COLOR_GREEN display_rgb(0, 200, 0)
#define COLOR_BLUE display_rgb(40, 120, 255)
#define COLOR_GREY display_rgb(190, 190, 190)
#define COLOR_AMBER display_rgb(255, 170, 0)

// Bring up the panel transport and brightness control. Returns false if init
// failed (caller then skips all UI). Allocates the framebuffer.
bool display_init(void);

// Fill the whole framebuffer with one color.
void display_clear(display_color_t color);

// Draw a NUL-terminated string at (x, y) top-left, integer-scaled by `scale`
// (1 = GLYPH_H pixels tall). Clipped to the framebuffer. No wrapping.
void display_text(int x, int y, int scale, display_color_t color,
                  const char *str);

// Same metrics, using the bold face (synthetic bold for 6x10).
void display_text_bold(int x, int y, int scale, display_color_t color,
                       const char *str);

// Fill a w*h rectangle at (x, y) with one color. Clipped to the framebuffer.
void display_fill_rect(int x, int y, int w, int h, display_color_t color);

// Set brightness, 0..100 percent (LCD PWM or OLED contrast; 0 turns it off).
void display_set_backlight(uint8_t percent);

// Blit an RGBA image (w*h*4 bytes, row-major) at (x, y). Pixels with alpha
// below ~16% are treated as transparent and skipped (background shows through).
// Colors go through display_rgb so they match the panel.
void display_blit_rgba(int x, int y, int w, int h, const uint8_t *rgba);

// Push the framebuffer to the panel.
void display_flush(void);
