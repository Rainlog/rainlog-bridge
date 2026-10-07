#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "activity.h"
#include "ap_clients.h"
#include "button.h"
#include "config_store.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_reset.h"
#include "screen_setup.h"
#include "screen_status.h"
#include "ui_common.h"
#include "upload_stats.h"
#include "wifi_link.h"

static const char *TAG = "ui";

#define REFRESH_MS 1000
#define RESET_REFRESH_MS 80  // smooth countdown while the button is held

// Backlight dims to BL_DIM_PCT after BL_DIM_AFTER_US with no activity, and
// returns to full on the next activity (cuts heat/power when no one is
// looking).
#define BL_FULL_PCT 100
#define BL_DIM_PCT 6
#define BL_DIM_AFTER_US (30 * 1000 * 1000)

#if BOARD_DISPLAY_SSD1306
#define OLED_COLUMNS (DISPLAY_W / GLYPH)
#define OLED_ROWS (DISPLAY_H / GLYPH_H)
#define OLED_ROW_HEIGHT (DISPLAY_H / OLED_ROWS)

static void oled_line(int row, const char *text) {
  char line[OLED_COLUMNS + 1];
  snprintf(line, sizeof(line), "%s", text);
  display_text(0, row * OLED_ROW_HEIGHT, 1, COLOR_WHITE, line);
}

static void oled_draw(uint32_t held) {
  char line[80];
  if (held >= RESET_ARM_MS) {
    oled_line(0, "FACTORY RESET");
    uint32_t remaining =
        held >= RESET_HOLD_MS ? 0 : (RESET_HOLD_MS - held) / 1000;
    snprintf(line, sizeof(line), "%lu seconds", (unsigned long)remaining);
    oled_line(1, line);
    oled_line(2, "Keep holding");
    oled_line(3, "Release cancels");
    return;
  }
  const bridge_config_t *cfg = config_get();
  if (!config_is_provisioned()) {
    // Cycle long credentials in screen-width chunks so the complete SSID
    // and password remain readable on the small display.
    unsigned page = (unsigned)(esp_timer_get_time() / 3000000);
    unsigned ssid_parts =
        (strlen(cfg->ap_ssid) + OLED_COLUMNS - 1) / OLED_COLUMNS;
    unsigned pass_parts =
        (strlen(cfg->ap_pass) + OLED_COLUMNS - 1) / OLED_COLUMNS;
    if (ssid_parts == 0) ssid_parts = 1;
    if (pass_parts == 0) pass_parts = 1;
#if OLED_ROWS >= 6
    oled_line(0, "Wi-Fi network:");
    oled_line(1, cfg->ap_ssid + (page % ssid_parts) * OLED_COLUMNS);
    oled_line(2, "Password:");
    oled_line(3, cfg->ap_pass + (page % pass_parts) * OLED_COLUMNS);
    oled_line(4, "Open in browser:");
    wifi_link_ap_ip_str(line, sizeof(line));
    oled_line(5, line);
#else
    oled_line(0, "JOIN BRIDGE WIFI");
    oled_line(1, cfg->ap_ssid + (page % ssid_parts) * OLED_COLUMNS);
    oled_line(2, cfg->ap_pass + (page % pass_parts) * OLED_COLUMNS);
    wifi_link_ap_ip_str(line, sizeof(line));
    oled_line(3, line);
#endif
  } else {
    oled_line(0, "Rainlog Bridge");
    snprintf(line, sizeof(line), "WiFi %s",
             wifi_link_sta_has_ip() ? "connected" : "offline");
    oled_line(1, line);
    snprintf(line, sizeof(line), "RL %lu Q %d",
             (unsigned long)upload_stats_total(UPLOAD_TARGET_RL),
             forwarder_pending_count());
    oled_line(2, line);
#if OLED_ROWS >= 6
    snprintf(line, sizeof(line), "WU %lu",
             (unsigned long)upload_stats_total(UPLOAD_TARGET_WU));
    oled_line(3, line);
    snprintf(line, sizeof(line), "Wi-Fi clients: %d",
             wifi_link_ap_station_count());
    oled_line(4, line);
    oled_line(5, forwarder_last_result());
#else
    oled_line(3, forwarder_last_result());
#endif
  }
}
#endif

static void ui_task(void *arg) {
  (void)arg;
  while (true) {
    // Factory-reset countdown takes over while the button is held past the arm
    // threshold; holding through it erases settings and reboots.
    uint32_t held = button_held_ms();
    if (held >= RESET_ARM_MS) {
      display_set_backlight(BL_FULL_PCT);
      display_clear(COLOR_BLACK);
#if BOARD_DISPLAY_SSD1306
      oled_draw(held);
#else
      screen_reset_draw(held);
#endif
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
#if BOARD_DISPLAY_SSD1306
    oled_draw(held);
#else
    ui_draw_header();

    // Pick the screen for the current state.
    if (!config_is_provisioned()) {
      screen_setup_draw();
    } else {
      screen_status_draw();
    }

#endif

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
