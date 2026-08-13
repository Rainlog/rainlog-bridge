#include "button.h"

#include "activity.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_MS 30  // also serves as debounce interval

// Updated by the task; a 32-bit read is atomic (no torn value that could
// spuriously trip the factory-reset hold).
static volatile uint32_t s_held_ms;

static void button_task(void *arg) {
  (void)arg;
  int prev = 1;  // active low: 1 = released
  int64_t press_start = 0;
  while (true) {
    int level = gpio_get_level(BOARD_BOOT_BUTTON_GPIO);
    if (level == 0) {  // held
      if (prev == 1) {
        press_start = esp_timer_get_time();
        activity_poke();  // any press wakes the backlight
      }
      int64_t held = esp_timer_get_time() - press_start;
      s_held_ms = held < 0 ? 0 : (uint32_t)(held / 1000);
    } else {  // released
      s_held_ms = 0;
    }
    prev = level;
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

uint32_t button_held_ms(void) { return s_held_ms; }

void button_init(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&cfg);
  xTaskCreate(button_task, "button", 2048, NULL, 4, NULL);
}
