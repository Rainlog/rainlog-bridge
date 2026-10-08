#include "display.h"

#include <string.h>

#include "display_panel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#if BOARD_DISPLAY_FONT == BOARD_FONT_6X10
#include "font6x10.h"
#define DISPLAY_FONT_REGULAR font6x10
#define DISPLAY_FONT_BOLD font6x10_bold
#else
#include "font8x13.h"
#define DISPLAY_FONT_REGULAR font8x13
#define DISPLAY_FONT_BOLD font8x13_bold
#endif

static const char *TAG = "display";
#define FB_PIXELS (DISPLAY_FB_BYTES / sizeof(display_color_t))
static int s_strip_y;
static int s_strip_rows = DISPLAY_H;
static display_color_t *s_fb;

display_color_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if BOARD_DISPLAY_SSD1306
  return (r | g | b) != 0;
#else
  // The LCD backend configures RAMCTRL for native RGB565 byte order.
  return (display_color_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
#endif
}

static void draw_blank(void *unused) {
  (void)unused;
  display_clear(COLOR_BLACK);
}

bool display_init(void) {
  if (display_panel_init() != ESP_OK) {
    ESP_LOGE(TAG, "panel initialization failed");
    return false;
  }
#if BOARD_DISPLAY_SSD1306
  s_fb = heap_caps_malloc(DISPLAY_FB_BYTES, MALLOC_CAP_8BIT);
#else
  s_fb = heap_caps_malloc(DISPLAY_FB_BYTES, MALLOC_CAP_DMA);
#endif
  if (s_fb == NULL) {
    ESP_LOGE(TAG, "render buffer allocation failed");
    return false;
  }
  display_render(draw_blank, NULL);
  display_set_backlight(100);
  ESP_LOGI(TAG, "Display up (%dx%d), free heap %u", DISPLAY_W, DISPLAY_H,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
  return true;
}

void display_clear(display_color_t color) {
  display_color_t *fb = s_fb;
  if (fb == NULL) {
    return;
  }
#if BOARD_DISPLAY_SSD1306
  memset(fb, color ? 0xff : 0, DISPLAY_FB_BYTES);
#else
  for (size_t i = 0; i < FB_PIXELS; i++) {
    fb[i] = color;
  }
#endif
}

static void put_pixel(int x, int y, display_color_t color) {
  if (s_fb == NULL || x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) {
    return;
  }
#if BOARD_DISPLAY_SSD1306
  uint8_t *page = &s_fb[(y / 8) * DISPLAY_W + x];
  uint8_t mask = 1U << (y % 8);
  if (color)
    *page |= mask;
  else
    *page &= (uint8_t)~mask;
#else
  if (y < s_strip_y || y >= s_strip_y + s_strip_rows) return;
  s_fb[(y - s_strip_y) * DISPLAY_W + x] = color;
#endif
}

static void draw_text(const uint8_t font[128][GLYPH_H], int x, int y, int scale,
                      display_color_t color, const char *str) {
  if (s_fb == NULL || scale < 1) {
    return;
  }
  int cursor_x = x;
  for (const char *p = str; *p != '\0'; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch >= 128) {
      ch = '?';
    }
    const uint8_t *glyph = font[ch];  // GLYPH_H rows, LSB = leftmost col
    for (int row = 0; row < GLYPH_H; row++) {
#if !BOARD_DISPLAY_SSD1306
      int top = y + row * scale;
      if (top + scale <= s_strip_y || top >= s_strip_y + s_strip_rows) continue;
#endif
      uint8_t bits = glyph[row];
      for (int col = 0; col < GLYPH; col++) {
        if (bits & (1 << col)) {
          for (int dy = 0; dy < scale; dy++) {
            for (int dx = 0; dx < scale; dx++) {
              put_pixel(cursor_x + col * scale + dx, y + row * scale + dy,
                        color);
            }
          }
        }
      }
    }
    cursor_x += GLYPH * scale;
  }
}

void display_text(int x, int y, int scale, display_color_t color,
                  const char *str) {
  draw_text(DISPLAY_FONT_REGULAR, x, y, scale, color, str);
}

void display_text_bold(int x, int y, int scale, display_color_t color,
                       const char *str) {
  draw_text(DISPLAY_FONT_BOLD, x, y, scale, color, str);
}

void display_fill_rect(int x, int y, int w, int h, display_color_t color) {
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      put_pixel(x + dx, y + dy, color);
    }
  }
}

void display_set_backlight(uint8_t percent) {
  if (percent > 100) percent = 100;
  esp_err_t err = display_panel_brightness(percent);
  if (err != ESP_OK) ESP_LOGE(TAG, "brightness: %s", esp_err_to_name(err));
}

void display_blit_rgba(int x, int y, int w, int h, const uint8_t *rgba) {
  if (s_fb == NULL) {
    return;
  }
  for (int row = 0; row < h; row++) {
#if !BOARD_DISPLAY_SSD1306
    if (y + row < s_strip_y || y + row >= s_strip_y + s_strip_rows) continue;
#endif
    for (int col = 0; col < w; col++) {
      const uint8_t *p = rgba + ((row * w + col) * 4);
      if (p[3] < 40) {
        continue;  // transparent
      }
      put_pixel(x + col, y + row, display_rgb(p[0], p[1], p[2]));
    }
  }
}

void display_blit_mono(int x, int y, int w, int h, const uint8_t *pages) {
  for (int row = 0; row < h; row++) {
#if !BOARD_DISPLAY_SSD1306
    if (y + row < s_strip_y || y + row >= s_strip_y + s_strip_rows) continue;
#endif
    for (int col = 0; col < w; col++) {
      bool lit = pages[(row / 8) * w + col] & (1U << (row % 8));
      put_pixel(x + col, y + row, lit ? COLOR_WHITE : COLOR_BLACK);
    }
  }
}

void display_render(void (*draw)(void *), void *context) {
  if (s_fb == NULL || draw == NULL) return;
#if BOARD_DISPLAY_SSD1306
  s_strip_rows = DISPLAY_H;
#else
  s_strip_rows = DISPLAY_STRIP_ROWS;
#endif
  for (s_strip_y = 0; s_strip_y < DISPLAY_H; s_strip_y += s_strip_rows) {
    int rows = DISPLAY_H - s_strip_y;
    if (rows > s_strip_rows) rows = s_strip_rows;
    draw(context);
    esp_err_t err = display_panel_flush(s_fb, s_strip_y, rows);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "flush row %d: %s", s_strip_y, esp_err_to_name(err));
      break;
    }
  }
}
