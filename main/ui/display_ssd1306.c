#include "board.h"
#include "display_panel.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "display_ssd1306";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static int s_brightness = -1;

esp_err_t display_panel_init(void) {
  i2c_master_bus_handle_t bus;
  i2c_master_bus_config_t bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "I2C bus");
  esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = BOARD_OLED_ADDRESS,
      .scl_speed_hz = 400000,
      .control_phase_bytes = 1,
      .dc_bit_offset = 6,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &s_io), TAG,
                      "OLED IO");
  esp_lcd_panel_ssd1306_config_t oled = {.height = BOARD_DISPLAY_H};
  esp_lcd_panel_dev_config_t config = {
      .reset_gpio_num = -1,
      .bits_per_pixel = 1,
      .vendor_config = &oled,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &config, &s_panel), TAG,
                      "OLED panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG,
                      "invert");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG,
                      "orientation");
  return esp_lcd_panel_disp_on_off(s_panel, true);
}

esp_err_t display_panel_flush(const display_color_t *pixels, int y, int rows) {
  // ESP-IDF's I2C panel IO transmits synchronously. The framebuffer is free
  // for reuse when this returns; no SPI-style DMA semaphore is needed.
  return esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_DISPLAY_W,
                                   y + rows, pixels);
}

esp_err_t display_panel_brightness(uint8_t percent) {
  if (s_brightness == percent) return ESP_OK;
  if (percent == 0 || s_brightness == 0) {
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, percent != 0), TAG,
                        "power");
  }
  if (percent != 0) {
    uint8_t contrast = (unsigned)percent * 255 / 100;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, 0x81, &contrast, 1),
                        TAG, "contrast");
  }
  s_brightness = percent;
  return ESP_OK;
}
