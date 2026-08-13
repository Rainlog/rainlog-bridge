#include "activity.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// 64-bit on a 32-bit RISC-V is two loads/stores; the spinlock guards against a
// torn read between the writer (button/config tasks) and the UI task.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_us;

void activity_init(void) { activity_poke(); }

void activity_poke(void) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&s_mux);
  s_last_us = now;
  portEXIT_CRITICAL(&s_mux);
}

int64_t activity_last_us(void) {
  portENTER_CRITICAL(&s_mux);
  int64_t v = s_last_us;
  portEXIT_CRITICAL(&s_mux);
  return v;
}
