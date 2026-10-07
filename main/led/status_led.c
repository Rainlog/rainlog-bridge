#include "status_led.h"

#include "board.h"
#include "config_store.h"
#include "driver/gpio.h"
#if !BOARD_DISPLAY_SSD1306
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !BOARD_DISPLAY_SSD1306
static const char *TAG = "status_led";
#endif

// RMT resolution: 10MHz -> 0.1us per tick. WS2812 bit timings (T0H 0.3us /
// T0L 0.9us, T1H 0.9us / T1L 0.3us) expressed in ticks.
#define RMT_RES_HZ (10 * 1000 * 1000)

#define SUCCESS_FLASH_US (250 * 1000)
#define RENDER_PERIOD_MS 50

#if !BOARD_DISPLAY_SSD1306
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
#endif

static volatile bool s_error;
static volatile bool s_provisioning;
// 64-bit, so guarded by a spinlock against torn reads on the 32-bit core
// (written by whichever task reports a success, read by the LED task).
static portMUX_TYPE s_success_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_success_until_us;

static int64_t success_until_us(void) {
  portENTER_CRITICAL(&s_success_mux);
  int64_t v = s_success_until_us;
  portEXIT_CRITICAL(&s_success_mux);
  return v;
}

static void render(uint8_t r, uint8_t g, uint8_t b) {
#if BOARD_DISPLAY_SSD1306
  gpio_set_level(BOARD_STATUS_LED_GPIO, r != 0 || g != 0 || b != 0);
#else
  if (s_chan == NULL || s_encoder == NULL) {
    return;
  }
  // This board's onboard LED uses RGB wire order, not the usual WS2812 GRB:
  // verified on hardware (intended red lit green until R/G were un-swapped;
  // blue was already correct). The Waveshare demo's GRB default goes unnoticed
  // because it only cycles a rainbow table, never a known primary.
  uint8_t buf[3] = {r, g, b};
  rmt_transmit_config_t tx = {.loop_count = 0};
  rmt_transmit(s_chan, s_encoder, buf, sizeof(buf), &tx);
  rmt_tx_wait_all_done(s_chan, portMAX_DELAY);
#endif
}

static void led_task(void *arg) {
  (void)arg;
  uint32_t last = 0xFFFFFFFF;  // force first render
  while (true) {
    uint8_t r = 0, g = 0, b = 0;
    if (s_error) {
      r = config_get()->led_level;  // red wins over everything
    } else if (esp_timer_get_time() < success_until_us()) {
      g = config_get()->led_level;  // green success pulse
    } else if (s_provisioning) {
      b = config_get()->led_level;  // blue
    }
    // Only push to the LED when the color actually changes, so the CPU can idle
    // (and DFS can drop frequency) instead of re-transmitting every tick.
    uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (color != last) {
      render(r, g, b);
      last = color;
    }
    vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS));
  }
}

void status_led_init(void) {
#if BOARD_DISPLAY_SSD1306
  gpio_config_t config = {
      .pin_bit_mask = 1ULL << BOARD_STATUS_LED_GPIO,
      .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&config));
#else
  rmt_tx_channel_config_t chan_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .gpio_num = BOARD_RGB_LED_GPIO,
      .mem_block_symbols = 64,
      .resolution_hz = RMT_RES_HZ,
      .trans_queue_depth = 4,
  };
  esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt channel init failed: %s", esp_err_to_name(err));
    s_chan = NULL;
    return;
  }

  rmt_bytes_encoder_config_t enc_cfg = {
      .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
      .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
      .flags.msb_first = 1,
  };
  err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt encoder init failed: %s", esp_err_to_name(err));
    s_encoder = NULL;
    return;
  }

  ESP_ERROR_CHECK(rmt_enable(s_chan));
#endif
  render(0, 0, 0);
  xTaskCreate(led_task, "status_led", 2048, NULL, 1, NULL);
}

void status_led_set_error(bool on) { s_error = on; }

void status_led_set_provisioning(bool on) { s_provisioning = on; }

void status_led_flash_success(void) {
  int64_t until = esp_timer_get_time() + SUCCESS_FLASH_US;
  portENTER_CRITICAL(&s_success_mux);
  s_success_until_us = until;
  portEXIT_CRITICAL(&s_success_mux);
}
