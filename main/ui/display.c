#include "display.h"

#include "display_panel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font8x13.h"

static const char *TAG = "display";
#define FB_PIXELS (DISPLAY_W * DISPLAY_H)
static uint16_t *s_fb;

uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
  // Keep RGB565 in native memory order; the LCD backend configures RAMCTRL
  // for this byte order, and the OLED backend packs it into monochrome pages.
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool display_init(void) {
  if (display_panel_init() != ESP_OK) {
    ESP_LOGE(TAG, "panel initialization failed");
    return false;
  }
  s_fb = heap_caps_malloc(FB_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (s_fb == NULL) {
    ESP_LOGE(TAG, "framebuffer allocation failed");
    return false;
  }
  display_clear(display_rgb(0, 0, 0));
  display_flush();
  display_set_backlight(100);
  ESP_LOGI(TAG, "Display up (%dx%d), free heap %u", DISPLAY_W, DISPLAY_H,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
  return true;
}

void display_clear(uint16_t color) {
  uint16_t *fb = s_fb;
  if (fb == NULL) {
    return;
  }
  for (int i = 0; i < FB_PIXELS; i++) {
    fb[i] = color;
  }
}

static void put_pixel(int x, int y, uint16_t color) {
  if (s_fb == NULL || x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) {
    return;
  }
  s_fb[y * DISPLAY_W + x] = color;
}

static void draw_text(const uint8_t font[128][GLYPH_H], int x, int y, int scale,
                      uint16_t color, const char *str) {
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

void display_text(int x, int y, int scale, uint16_t color, const char *str) {
  draw_text(font8x13, x, y, scale, color, str);
}

void display_text_bold(int x, int y, int scale, uint16_t color,
                       const char *str) {
  draw_text(font8x13_bold, x, y, scale, color, str);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
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
    for (int col = 0; col < w; col++) {
      const uint8_t *p = rgba + ((row * w + col) * 4);
      if (p[3] < 40) {
        continue;  // transparent
      }
      put_pixel(x + col, y + row, display_rgb(p[0], p[1], p[2]));
    }
  }
}

void display_flush(void) {
  if (s_fb == NULL) return;
  esp_err_t err = display_panel_flush(s_fb);
  if (err != ESP_OK) ESP_LOGE(TAG, "flush: %s", esp_err_to_name(err));
}
