#include "display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "display_panel.h"
#include "oled_logo.h"
static display_color_t frame[DISPLAY_FB_BYTES / sizeof(display_color_t)];
size_t framebuffer_allocation;
static display_color_t pixel_at(int index) {
#if BOARD_DISPLAY_SSD1306
  int x = index % DISPLAY_W, y = index / DISPLAY_W;
  return (frame[(y / 8) * DISPLAY_W + x] >> (y % 8)) & 1;
#else
  return frame[index];
#endif
}
static int brightness;

esp_err_t display_panel_init(void) { return ESP_OK; }
esp_err_t display_panel_flush(const display_color_t *pixels) {
  memcpy(frame, pixels, sizeof(frame));
  return ESP_OK;
}
esp_err_t display_panel_brightness(uint8_t percent) {
  brightness = percent;
  return ESP_OK;
}

int main(void) {
  // Drawing before init must be harmless (a headless device can skip init).
  display_fill_rect(0, 0, 1, 1, COLOR_WHITE);
  assert(display_init());
  assert(brightness == 100);
  assert(framebuffer_allocation == DISPLAY_FB_BYTES);
#if BOARD_DISPLAY_SSD1306
  assert(DISPLAY_FB_BYTES == 1024);
  assert(display_rgb(1, 0, 0) == 1);
  assert(display_rgb(0, 0, 0) == 0);
#else
  assert(display_rgb(255, 0, 0) == 0xf800);
  assert(display_rgb(0, 255, 0) == 0x07e0);
  assert(display_rgb(0, 0, 255) == 0x001f);
#endif
  display_clear(COLOR_BLACK);
  display_fill_rect(-1, -1, 2, 2, COLOR_RED);
  display_fill_rect(DISPLAY_W - 1, DISPLAY_H - 1, 3, 3, COLOR_BLUE);
  display_flush();
  assert(pixel_at(0) == COLOR_RED);
  assert(pixel_at(1) == COLOR_BLACK);
  assert(pixel_at(DISPLAY_W) == COLOR_BLACK);
  assert(pixel_at(DISPLAY_W * DISPLAY_H - 1) == COLOR_BLUE);

  // The selected logo must reproduce its packed asset without changing
  // adjacent content, including its trimmed 14-pixel width.
  display_clear(COLOR_BLACK);
  display_blit_mono(0, 0, OLED_LOGO_W, OLED_LOGO_H, oled_logo_pages);
  display_flush();
  assert(OLED_LOGO_W == 14 && OLED_LOGO_H == 64);
  for (int y = 0; y < OLED_LOGO_H; y++) {
    for (int x = 0; x < OLED_LOGO_W; x++) {
      bool lit = (oled_logo_pages[(y / 8) * OLED_LOGO_W + x] >> (y % 8)) & 1;
      assert(pixel_at(y * DISPLAY_W + x) == (lit ? COLOR_WHITE : COLOR_BLACK));
    }
    assert(pixel_at(y * DISPLAY_W + OLED_LOGO_W) == COLOR_BLACK);
  }

  // Packed bitmap placement crosses page boundaries, clears zero bits and
  // clips its last column at the display edge.
  const uint8_t mono[] = {0x81, 0x02};
  display_clear(COLOR_WHITE);
  display_blit_mono(DISPLAY_W - 1, 3, 2, 8, mono);
  display_flush();
  assert(pixel_at(3 * DISPLAY_W + DISPLAY_W - 1) == COLOR_WHITE);
  assert(pixel_at(4 * DISPLAY_W + DISPLAY_W - 1) == COLOR_BLACK);
  assert(pixel_at(10 * DISPLAY_W + DISPLAY_W - 1) == COLOR_WHITE);
  assert(pixel_at(4 * DISPLAY_W + DISPLAY_W - 2) == COLOR_WHITE);
  display_clear(COLOR_BLACK);

  const uint8_t image[] = {0, 255, 0, 255, 255, 0, 0, 39};
  display_blit_rgba(0, 0, 2, 1, image);
  display_flush();
  assert(pixel_at(0) == display_rgb(0, 255, 0));
  assert(pixel_at(1) == COLOR_BLACK);
  display_set_backlight(255);
  assert(brightness == 100);
  display_set_backlight(0);
  assert(brightness == 0);

  // A full row must fit in the selected cell width, including its final
  // glyph. Drawing one extra glyph exercises clipping at the right edge.
  char text[DISPLAY_W / GLYPH + 2];
  memset(text, 'A', sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  display_clear(COLOR_BLACK);
  int y = DISPLAY_H - GLYPH_H;
  display_text(0, y, 1, COLOR_WHITE, text);
  display_flush();
  int lit = 0;
  int last_x = (DISPLAY_W / GLYPH - 1) * GLYPH;
  for (int row = y; row < DISPLAY_H; row++) {
    for (int x = last_x; x < last_x + GLYPH; x++) {
      lit += pixel_at(row * DISPLAY_W + x) != COLOR_BLACK;
    }
  }
  assert(lit > 0);
#if BOARD_DISPLAY_FONT == BOARD_FONT_6X10
  assert(GLYPH == 6 && GLYPH_H == 10);
#if BOARD_DISPLAY_SSD1306
  // Eighteen characters fit from x=21 with the last cell's blank column
  // clipped. Verify the final glyph still has its complete five-pixel face.
  display_clear(COLOR_BLACK);
  display_text(21, 0, 1, COLOR_WHITE, "AAAAAAAAAAAAAAAAAA");
  display_flush();
  assert(pixel_at(3 * DISPLAY_W + 123) == COLOR_WHITE);
  assert(pixel_at(3 * DISPLAY_W + 127) == COLOR_WHITE);
  display_clear(COLOR_BLACK);
  display_text(0, y, 1, COLOR_WHITE, text);
  display_flush();
#endif
  // Known X11 6x10 'A' strokes verify bit order, cell advance and scaling.
  assert(pixel_at((y + 1) * DISPLAY_W + 2) == COLOR_WHITE);
  assert(pixel_at((y + 1) * DISPLAY_W + 3) == COLOR_BLACK);
  assert(pixel_at((y + 1) * DISPLAY_W + 8) == COLOR_WHITE);
  assert(pixel_at((y + 3) * DISPLAY_W) == COLOR_WHITE);
  assert(pixel_at((y + 3) * DISPLAY_W + 4) == COLOR_WHITE);
  display_clear(COLOR_BLACK);
  display_text(1, 1, 2, COLOR_WHITE, "A");
  display_flush();
  assert(pixel_at(3 * DISPLAY_W + 5) == COLOR_WHITE);
  assert(pixel_at(4 * DISPLAY_W + 6) == COLOR_WHITE);
  assert(pixel_at(3 * DISPLAY_W + 7) == COLOR_BLACK);
  display_clear(COLOR_BLACK);
  display_text_bold(0, 0, 1, COLOR_WHITE, "A");
  display_flush();
  assert(pixel_at(DISPLAY_W + 2) == COLOR_WHITE);
  assert(pixel_at(DISPLAY_W + 3) == COLOR_WHITE);
#endif

#if BOARD_DISPLAY_SSD1306
  display_clear(COLOR_BLACK);
  display_fill_rect(0, 0, 1, 1, COLOR_RED);
  display_fill_rect(1, 7, 1, 1, COLOR_BLUE);
  display_fill_rect(2, 8, 1, 1, COLOR_GREEN);
  display_fill_rect(127, 63, 1, 1, COLOR_WHITE);
  display_flush();
  assert(frame[0] == 0x01);
  assert(frame[1] == 0x80);
  assert(frame[2] == 0);
  assert(frame[128 + 2] == 0x01);
  assert(frame[1023] == 0x80);
  unsigned count = 0;
  for (unsigned i = 0; i < sizeof(frame); i++) count += frame[i] != 0;
  assert(count == 4);
  display_fill_rect(0, 1, 1, 1, COLOR_WHITE);
  display_fill_rect(0, 0, 1, 1, COLOR_BLACK);
  display_flush();
  assert(frame[0] == 0x02);
  display_clear(COLOR_WHITE);
  display_flush();
  for (unsigned i = 0; i < sizeof(frame); i++) assert(frame[i] == 0xff);
  display_clear(COLOR_BLACK);
  display_flush();
  for (unsigned i = 0; i < sizeof(frame); i++) assert(frame[i] == 0);
#endif
  puts("Display rendering and pixel format checks passed");
  return 0;
}
