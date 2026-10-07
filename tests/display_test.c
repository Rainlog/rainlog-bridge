#include "display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "display_panel.h"
#if BOARD_DISPLAY_SSD1306
#include "ssd1306_pixels.h"
#endif

static uint16_t frame[DISPLAY_W * DISPLAY_H];
static int brightness;

esp_err_t display_panel_init(void) { return ESP_OK; }
esp_err_t display_panel_flush(const uint16_t *pixels) {
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
  assert(display_rgb(255, 0, 0) == 0xf800);
  assert(display_rgb(0, 255, 0) == 0x07e0);
  assert(display_rgb(0, 0, 255) == 0x001f);
  display_clear(COLOR_BLACK);
  display_fill_rect(-1, -1, 2, 2, COLOR_RED);
  display_fill_rect(DISPLAY_W - 1, DISPLAY_H - 1, 3, 3, COLOR_BLUE);
  display_flush();
  assert(frame[0] == COLOR_RED);
  assert(frame[1] == COLOR_BLACK);
  assert(frame[DISPLAY_W] == COLOR_BLACK);
  assert(frame[DISPLAY_W * DISPLAY_H - 1] == COLOR_BLUE);

  const uint8_t image[] = {0, 255, 0, 255, 255, 0, 0, 39};
  display_blit_rgba(0, 0, 2, 1, image);
  display_flush();
  assert(frame[0] == display_rgb(0, 255, 0));
  assert(frame[1] == COLOR_BLACK);
  display_set_backlight(255);
  assert(brightness == 100);
  display_set_backlight(0);
  assert(brightness == 0);

#if BOARD_DISPLAY_SSD1306
  uint8_t pages[DISPLAY_W * DISPLAY_H / 8];
  display_clear(COLOR_BLACK);
  display_fill_rect(0, 0, 1, 1, COLOR_RED);
  display_fill_rect(1, 7, 1, 1, COLOR_BLUE);
  display_fill_rect(2, 8, 1, 1, COLOR_GREEN);
  display_fill_rect(127, 63, 1, 1, COLOR_WHITE);
  display_flush();
  memset(pages, 0xff, sizeof(pages));
  ssd1306_pack_pixels(frame, pages);
  assert(pages[0] == 0x01);
  assert(pages[1] == 0x80);
  assert(pages[2] == 0);
  assert(pages[128 + 2] == 0x01);
  assert(pages[1023] == 0x80);
  unsigned count = 0;
  for (unsigned i = 0; i < sizeof(pages); i++) count += pages[i] != 0;
  assert(count == 4);
  display_clear(COLOR_WHITE);
  display_flush();
  ssd1306_pack_pixels(frame, pages);
  for (unsigned i = 0; i < sizeof(pages); i++) assert(pages[i] == 0xff);
  display_clear(COLOR_BLACK);
  display_flush();
  ssd1306_pack_pixels(frame, pages);
  for (unsigned i = 0; i < sizeof(pages); i++) assert(pages[i] == 0);
#endif
  puts("Display rendering and pixel format checks passed");
  return 0;
}
