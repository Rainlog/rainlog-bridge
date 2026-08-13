#include "display.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "font8x13.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "st7789t_panel.h"

static const char *TAG = "display";

// Panel geometry (Waveshare ESP32-C6-LCD-1.47, from the demo driver):
// 172x320 visible, offset 34 columns into the ST7789's 240-wide RAM.
#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (12 * 1000 * 1000)
#define LCD_X_OFFSET 34
#define FB_PIXELS (DISPLAY_W * DISPLAY_H)

// Backlight PWM (LEDC), so brightness can be dimmed when idle.
#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES LEDC_TIMER_13_BIT
#define BL_LEDC_MAX_DUTY ((1 << 13) - 1)

static esp_lcd_panel_handle_t s_panel;

// Single shared framebuffer. A second buffer would cost another ~110KB of the
// C6's small SRAM (no PSRAM on this board), starving the heap that TLS to
// rainlog.org needs (handshake OOM was breaking forwarding + OTA). The UI only
// redraws at ~1Hz and display_flush waits for the previous DMA to finish, so by
// the time the next frame is composed the panel transfer is long done: no
// tearing despite the single buffer. Both indices point at the same memory so
// the existing swap logic stays a harmless no-op.
static uint16_t *s_fb[2];
static int s_back;
static SemaphoreHandle_t s_flush_done;  // given when a panel transfer completes

uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
  // Standard RGB565, no byte swap: esp_lcd handles the SPI byte order, and the
  // panel config's element order makes plain RGB values display correctly
  // (verified on hardware with a 4-packing swatch test).
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx) {
  (void)io;
  (void)edata;
  (void)ctx;
  BaseType_t hp_task_woken = pdFALSE;
  xSemaphoreGiveFromISR(s_flush_done, &hp_task_woken);
  return hp_task_woken == pdTRUE;
}

bool display_init(void) {
  s_flush_done = xSemaphoreCreateBinary();
  // Available initially so the first flush doesn't block waiting on a transfer
  // that never happened.
  xSemaphoreGive(s_flush_done);

  spi_bus_config_t buscfg = {
      .sclk_io_num = BOARD_SPI_SCLK_GPIO,
      .mosi_io_num = BOARD_SPI_MOSI_GPIO,
      .miso_io_num = BOARD_SPI_MISO_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = FB_PIXELS * (int)sizeof(uint16_t),
  };
  if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed");
    return false;
  }

  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = BOARD_LCD_DC_GPIO,
      .cs_gpio_num = BOARD_LCD_CS_GPIO,
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 10,
      .on_color_trans_done = on_trans_done,
  };
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config,
                               &io) != ESP_OK) {
    ESP_LOGE(TAG, "panel io init failed");
    return false;
  }

  esp_lcd_panel_dev_st7789t_config_t panel_config = {
      .reset_gpio_num = BOARD_LCD_RST_GPIO,
      .rgb_endian = LCD_RGB_ELEMENT_ORDER_BGR,
      .bits_per_pixel = 16,
  };
  if (esp_lcd_new_panel_st7789t(io, &panel_config, &s_panel) != ESP_OK) {
    ESP_LOGE(TAG, "panel init failed");
    return false;
  }

  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  esp_lcd_panel_mirror(s_panel, true, false);
  esp_lcd_panel_set_gap(s_panel, LCD_X_OFFSET, 0);
  esp_lcd_panel_disp_on_off(s_panel, true);

  // Backlight via LEDC PWM so it can be dimmed when idle.
  ledc_timer_config_t bl_timer = {
      .speed_mode = BL_LEDC_MODE,
      .timer_num = BL_LEDC_TIMER,
      .duty_resolution = BL_LEDC_RES,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&bl_timer);
  ledc_channel_config_t bl_chan = {
      .gpio_num = BOARD_LCD_BL_GPIO,
      .speed_mode = BL_LEDC_MODE,
      .channel = BL_LEDC_CHANNEL,
      .timer_sel = BL_LEDC_TIMER,
      .duty = 0,
      .hpoint = 0,
  };
  ledc_channel_config(&bl_chan);
  display_set_backlight(100);

  // One framebuffer, shared by both indices (see s_fb comment): saves ~110KB.
  s_fb[0] = heap_caps_malloc(FB_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (s_fb[0] == NULL) {
    ESP_LOGE(TAG, "framebuffer alloc failed");
    return false;
  }
  s_fb[1] = s_fb[0];
  display_clear(display_rgb(0, 0, 0));
  display_flush();
  ESP_LOGI(TAG, "LCD up (%dx%d), free heap %u", DISPLAY_W, DISPLAY_H,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
  return true;
}

void display_clear(uint16_t color) {
  uint16_t *fb = s_fb[s_back];
  if (fb == NULL) {
    return;
  }
  for (int i = 0; i < FB_PIXELS; i++) {
    fb[i] = color;
  }
}

static void put_pixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) {
    return;
  }
  s_fb[s_back][y * DISPLAY_W + x] = color;
}

static void draw_text(const uint8_t font[128][GLYPH_H], int x, int y, int scale,
                      uint16_t color, const char *str) {
  if (s_fb[s_back] == NULL || scale < 1) {
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
  if (percent > 100) {
    percent = 100;
  }
  uint32_t duty = (uint32_t)percent * BL_LEDC_MAX_DUTY / 100;
  ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
  ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

void display_blit_rgba(int x, int y, int w, int h, const uint8_t *rgba) {
  if (s_fb[s_back] == NULL) {
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
  if (s_panel == NULL || s_fb[s_back] == NULL) {
    return;
  }
  // Wait for the previous transfer to finish before handing the panel a new
  // buffer, then send the just-composed back buffer and swap so the next frame
  // composes into the now-free buffer while this one DMAs out.
  xSemaphoreTake(s_flush_done, portMAX_DELAY);
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_W, DISPLAY_H, s_fb[s_back]);
  s_back ^= 1;
}
