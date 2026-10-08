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

#if BOARD_DISPLAY_SSD1306
#include "oled_logo.h"
#define OLED_TEXT_X (OLED_LOGO_W + 1 + GLYPH)
#if BOARD_DISPLAY_FONT == BOARD_FONT_6X10
// Regular 6x10 glyphs have a blank sixth column. The final cell can omit it.
#define OLED_COLUMNS ((DISPLAY_W - OLED_TEXT_X + 1) / GLYPH)
#else
#define OLED_COLUMNS ((DISPLAY_W - OLED_TEXT_X) / GLYPH)
#endif
#define OLED_ROWS (DISPLAY_H / GLYPH_H)
#define OLED_ROW_HEIGHT (DISPLAY_H / OLED_ROWS)

static void oled_line(int row, const char *text) {
  char line[OLED_COLUMNS + 1];
  snprintf(line, sizeof(line), "%s", text);
  display_text(OLED_TEXT_X, row * OLED_ROW_HEIGHT, 1, COLOR_WHITE, line);
}

static void oled_draw(uint32_t held) {
  display_blit_mono(0, 0, OLED_LOGO_W, OLED_LOGO_H, oled_logo_pages);
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

typedef struct {
  uint32_t held;
  bool provisioned;
} ui_frame_t;

static void draw_frame(void *context) {
  const ui_frame_t *frame = context;
  display_clear(COLOR_BLACK);
#if BOARD_DISPLAY_SSD1306
  oled_draw(frame->held);
#else
  if (frame->held >= RESET_ARM_MS) {
    screen_reset_draw(frame->held);
  } else {
    ui_draw_header();
    if (frame->provisioned) screen_status_draw();
    else screen_setup_draw();
  }
#endif
}

static void ui_task(void *arg) {
  (void)arg;
  while (true) {
    // Factory-reset countdown takes over while the button is held past the arm
    // threshold; holding through it erases settings and reboots.
    uint32_t held = button_held_ms();
    if (held >= RESET_ARM_MS) {
      display_set_backlight(config_get()->display_full_pct);
      ui_frame_t frame = {.held = held};
      display_render(draw_frame, &frame);
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

    ui_frame_t frame = {.held = held, .provisioned = config_is_provisioned()};
#if !BOARD_DISPLAY_SSD1306
    if (frame.provisioned) screen_status_prepare();
#endif
    display_render(draw_frame, &frame);

    // Dim the backlight when idle (no recent activity: button or settings).
    int64_t idle_us = esp_timer_get_time() - activity_last_us();
    const bridge_config_t *cfg = config_get();
    display_set_backlight(cfg->display_dim_after_s &&
                                  idle_us > (int64_t)cfg->display_dim_after_s *
                                                1000000
                              ? cfg->display_dim_pct
                              : cfg->display_full_pct);

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
