#include "ui.h"

#include "activity.h"
#include "ap_clients.h"
#include "button.h"
#include "config_store.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_reset.h"
#include "screen_setup.h"
#include "screen_status.h"
#include "ui_common.h"
#include "upload_stats.h"

static const char *TAG = "ui";

#define REFRESH_MS 1000
#define RESET_REFRESH_MS 80  // smooth countdown while the button is held

// Backlight dims to BL_DIM_PCT after BL_DIM_AFTER_US with no activity, and
// returns to full on the next activity (cuts heat/power when no one is
// looking).
#define BL_FULL_PCT 100
#define BL_DIM_PCT 6
#define BL_DIM_AFTER_US (30 * 1000 * 1000)

static void ui_task(void *arg) {
  (void)arg;
  while (true) {
    // Factory-reset countdown takes over while the button is held past the arm
    // threshold; holding through it erases settings and reboots.
    uint32_t held = button_held_ms();
    if (held >= RESET_ARM_MS) {
      display_set_backlight(BL_FULL_PCT);
      display_clear(COLOR_BLACK);
      screen_reset_draw(held);
      display_flush();
      if (held >= RESET_HOLD_MS) {
        ESP_LOGW(TAG, "factory reset: clearing config + stats, rebooting");
        config_clear();
        upload_stats_clear();
        ap_clients_clear_names();
        esp_restart();
      }
      vTaskDelay(pdMS_TO_TICKS(RESET_REFRESH_MS));
      continue;
    }

    display_clear(COLOR_BLACK);
    ui_draw_header();

    // Pick the screen for the current state.
    if (!config_is_provisioned()) {
      screen_setup_draw();
    } else {
      screen_status_draw();
    }

    display_flush();

    // Dim the backlight when idle (no recent activity: button or settings).
    int64_t idle_us = esp_timer_get_time() - activity_last_us();
    display_set_backlight(idle_us > BL_DIM_AFTER_US ? BL_DIM_PCT : BL_FULL_PCT);

    vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
  }
}

void ui_start(void) {
  if (!display_init()) {
    ESP_LOGW(TAG, "display init failed; running headless");
    return;
  }
  // 4096 was enough before the status screen grew the ap_clients snapshot
  // (~1.1KB of ap_client_t on this stack, plus the wifi sta lists inside
  // ap_clients_snapshot).
  xTaskCreate(ui_task, "ui", 6144, NULL, 3, NULL);
}
