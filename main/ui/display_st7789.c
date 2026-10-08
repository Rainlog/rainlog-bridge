#include "board.h"
#include "display_panel.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display_st7789";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done;
#define BL_MAX_DUTY ((1 << 13) - 1)

static bool transfer_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event, void *ctx) {
  (void)io;
  (void)event;
  (void)ctx;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_done, &woken);
  return woken == pdTRUE;
}

esp_err_t display_panel_init(void) {
  s_done = xSemaphoreCreateBinary();
  ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "transfer semaphore");
  spi_bus_config_t bus = {
      .sclk_io_num = BOARD_SPI_SCLK_GPIO,
      .mosi_io_num = BOARD_SPI_MOSI_GPIO,
      .miso_io_num = BOARD_SPI_MISO_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = DISPLAY_FB_BYTES,
  };
  ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG,
                      "SPI bus");
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = BOARD_LCD_DC_GPIO,
      .cs_gpio_num = BOARD_LCD_CS_GPIO,
      .pclk_hz = 12000000,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 10,
      .on_color_trans_done = transfer_done,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                          (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io),
                      TAG, "panel IO");
  esp_lcd_panel_dev_config_t config = {
      .reset_gpio_num = BOARD_LCD_RST_GPIO,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
      .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
      .bits_per_pixel = 16,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &config, &s_panel), TAG,
                      "panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
  // Preserve Waveshare RAMCTRL (0x00, 0xE8), including native little-endian
  // RGB565, along with its voltage, porch and gamma settings. MADCTL and COLMOD
  // remain owned by ESP-IDF so its mirror and color state stays consistent.
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_tx_param(io, 0xB0, (uint8_t[]){0x00, 0xE8}, 2), TAG,
      "panel tuning");
  /* Porch Setting */
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_tx_param(io, 0xB2,
                                (uint8_t[]){0x0c, 0x0c, 0x00, 0x33, 0x33}, 5),
      TAG, "panel tuning");
  /* Gate Control, Vgh=13.65V, Vgl=-10.43V */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]){0x75}, 1),
                      TAG, "panel tuning");
  /* VCOM Setting, VCOM=1.175V */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]){0x1A}, 1),
                      TAG, "panel tuning");
  /* LCM Control, XOR: BGR, MX, MH */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x80}, 1),
                      TAG, "panel tuning");
  /* VDV and VRH Command Enable, enable=1 */
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]){0x01, 0xff}, 2), TAG,
      "panel tuning");
  /* VRH Set, Vap=4.4+... */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]){0x13}, 1),
                      TAG, "panel tuning");
  /* VDV Set, VDV=0 */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]){0x20}, 1),
                      TAG, "panel tuning");
  /* Frame Rate Control, 60Hz, inversion=0 */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]){0x0F}, 1),
                      TAG, "panel tuning");
  // Retain the original one-byte D0 write in this driver migration.
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]){0xA4, 0xA1}, 1), TAG,
      "panel tuning");
  /* Positive Voltage Gamma Control */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(
                          io, 0xE0,
                          (uint8_t[]){0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
                                      0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30},
                          14),
                      TAG, "panel tuning");
  /* Negative Voltage Gamma Control */
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(
                          io, 0xE1,
                          (uint8_t[]){0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
                                      0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E},
                          14),
                      TAG, "panel tuning");

  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG,
                      "mirror");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 34, 0), TAG, "gap");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                      "display on");
  ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .timer_num = LEDC_TIMER_0,
      .duty_resolution = LEDC_TIMER_13_BIT,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
  ledc_channel_config_t channel = {
      .gpio_num = BOARD_LCD_BL_GPIO,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
  };
  return ledc_channel_config(&channel);
}

esp_err_t display_panel_flush(const display_color_t *pixels, int y, int rows) {
  esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_DISPLAY_W,
                                            y + rows, pixels);
  if (err == ESP_OK) xSemaphoreTake(s_done, portMAX_DELAY);
  return err;
}

esp_err_t display_panel_brightness(uint8_t percent) {
  esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                (uint32_t)percent * BL_MAX_DUTY / 100);
  if (err != ESP_OK) return err;
  return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
