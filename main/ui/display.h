// Rainlog Wireless Bridge - LCD display primitives.
//
// Drives the onboard ST7789 (172x320) via esp_lcd and renders text from an
// 8x13 bitmap font (X11 misc-fixed, see font8x13.h) into an in-RAM
// framebuffer. Caller composes a frame with display_clear/display_text, then
// display_flush pushes it to the panel.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_W 172
#define DISPLAY_H 320

// 8x13 glyph cell: text at scale s advances GLYPH*s px per char and is
// GLYPH_H*s px tall.
#define GLYPH 8
#define GLYPH_H 13

// RGB565 helpers. The panel is wired BGR and wants byte-swapped 16-bit pixels;
// display_rgb() packs both so callers pass intuitive (r,g,b). See display.c.
uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b);

// Common colors (lazily-evaluated via display_rgb in callers is fine, but these
// macros keep call sites readable).
#define COLOR_BLACK display_rgb(0, 0, 0)
#define COLOR_WHITE display_rgb(255, 255, 255)
#define COLOR_RED display_rgb(255, 0, 0)
#define COLOR_GREEN display_rgb(0, 200, 0)
#define COLOR_BLUE display_rgb(40, 120, 255)
#define COLOR_GREY display_rgb(190, 190, 190)
#define COLOR_AMBER display_rgb(255, 170, 0)

// Bring up SPI + panel + backlight. Returns false if init failed (caller then
// skips all UI). Allocates the framebuffer.
bool display_init(void);

// Fill the whole framebuffer with one color.
void display_clear(uint16_t color);

// Draw a NUL-terminated string at (x, y) top-left, integer-scaled by `scale`
// (1 = 13px tall). Clipped to the framebuffer. No wrapping.
void display_text(int x, int y, int scale, uint16_t color, const char *str);

// Same, in the bold face (misc-fixed 8x13B). Same metrics as display_text.
void display_text_bold(int x, int y, int scale, uint16_t color,
                       const char *str);

// Fill a w*h rectangle at (x, y) with one color. Clipped to the framebuffer.
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

// Set LCD backlight brightness, 0..100 percent (PWM via LEDC).
void display_set_backlight(uint8_t percent);

// Blit an RGBA image (w*h*4 bytes, row-major) at (x, y). Pixels with alpha
// below ~16% are treated as transparent and skipped (background shows through).
// Colors go through display_rgb so they match the panel.
void display_blit_rgba(int x, int y, int w, int h, const uint8_t *rgba);

// Push the framebuffer to the panel.
void display_flush(void);
