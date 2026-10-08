#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "activity.h"
#include "ap_clients.h"
#include "button.h"
#include "config_store.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_update.h"
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
#include "radio/radio.h"
#define OLED_TEXT_X 2
#define OLED_TEXT_W (DISPLAY_W - 2 * OLED_TEXT_X)
#define OLED_VERSION_RIGHT 7  // Side margin plus five pixels of extra inset.
#define OLED_CONTENT_Y (15 + GLYPH_H + 4)
#if BOARD_DISPLAY_FONT == BOARD_FONT_6X10
// Regular 6x10 glyphs have a blank sixth column. The final cell can omit it.
#define OLED_COLUMNS ((OLED_TEXT_W + 1) / GLYPH)
#else
#define OLED_COLUMNS (OLED_TEXT_W / GLYPH)
#endif
#define OLED_ROW_HEIGHT (GLYPH_H + 1)
#define OLED_SECTION_GAP 3  // C6's 8-pixel section gap scaled to this panel.
// Reserve underline padding for three headings and two section gaps.
#define OLED_ROWS \
  ((DISPLAY_H - OLED_CONTENT_Y - 9 - 2 * OLED_SECTION_GAP + 1) / OLED_ROW_HEIGHT)
static int oled_section_offset;
static int oled_row_limit = OLED_ROWS;

static void oled_line(int row, const char *text) {
  if (row < 0 || row >= oled_row_limit) return;
  char line[OLED_COLUMNS + 1];
  snprintf(line, sizeof(line), "%s", text);
  display_text(OLED_TEXT_X, OLED_CONTENT_Y + row * OLED_ROW_HEIGHT + oled_section_offset, 1, COLOR_WHITE, line);
}

#if BOARD_DISPLAY_FONT == BOARD_FONT_4X6
static void oled_wifi_line(int row, const char *ssid, bool associated, int rssi) {
  if (row < 0 || row >= oled_row_limit) return;
  // Four compact bars occupy the rightmost eleven pixels of this 64px panel.
  char name[OLED_COLUMNS + 1];
  const int bars_x = DISPLAY_W - OLED_TEXT_X - 11;
  snprintf(name, sizeof(name), "%.*s", (bars_x - OLED_TEXT_X - 2) / GLYPH, ssid);
  oled_line(row, name);
  if (!associated) return;
  int level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  int bottom = OLED_CONTENT_Y + row * OLED_ROW_HEIGHT + oled_section_offset + GLYPH_H;
  for (int i = 0; i < 4; i++) {
    int height = i + 2;
    if (i < level)
      display_fill_rect(bars_x + i * 3, bottom - height, 2, height, COLOR_WHITE);
    else
      display_fill_rect(bars_x + i * 3, bottom - 1, 2, 1, COLOR_WHITE);
  }
}

static int oled_section(int row, const char *label) {
  oled_line(row, label);
  int y = OLED_CONTENT_Y + row * OLED_ROW_HEIGHT + oled_section_offset + GLYPH_H + 1;
  display_fill_rect(OLED_TEXT_X, y, OLED_TEXT_W, 1, COLOR_WHITE);
  oled_section_offset += 3;
  return row + 1;
}

// C6 status sections condensed into the portrait OLED's 16-column rows.
static void oled_status_dense(void) {
  const bridge_config_t *cfg = config_get();
  char line[80], ssid[33];
  int rssi = 0, row = 0;
  int pending = forwarder_pending_count();
  bool update = ota_update_available();
  radio_status_t radio;
  radio_status(&radio);
  radio_sensor_t sensors[RADIO_SENSORS_MAX];
  size_t count = radio_sensors(sensors, RADIO_SENSORS_MAX);
  ap_client_t clients[AP_CLIENTS_MAX];
  int nc = ap_clients_snapshot(clients, AP_CLIENTS_MAX);
  int client_rows = 0;
  for (int i = 0; i < nc; i++)
    if (clients[i].last_upload_us || clients[i].connected) client_rows++;
  int wanted_rows = 3 + (wifi_link_ap_enabled() ? 4 : 2) + 3 +
                    (cfg->wu_map_count ? 1 : 0) + 1 + client_rows + (int)count +
                    (pending ? 1 : 0) + (update ? 1 : 0);
  int section_gap = wanted_rows >= 13 ? 0 : OLED_SECTION_GAP;
  oled_row_limit = (DISPLAY_H - OLED_CONTENT_Y - 9 - 2 * section_gap + 1) / OLED_ROW_HEIGHT;
  int end = oled_row_limit - (pending ? 1 : 0) - (update ? 1 : 0);
  bool associated = wifi_link_sta_ap_info(ssid, sizeof(ssid), &rssi);
  row = oled_section(row, "Home Wi-Fi");
  oled_wifi_line(row++, associated ? ssid : cfg->sta_ssid, associated, rssi);
  if (wifi_link_sta_has_ip()) {
    char ip[16];
    wifi_link_sta_ip_str(ip, sizeof(ip));
    snprintf(line, sizeof(line), "%s", ip);
  } else
    snprintf(line, sizeof(line), "Wi-Fi offline");
  oled_line(row++, line);
  oled_section_offset += section_gap;
  row = oled_section(row, "Bridge Wi-Fi");
  if (wifi_link_ap_enabled()) {
    snprintf(line, sizeof(line), "%s", cfg->ap_ssid);
    oled_line(row++, line);
    char ip[16];
    wifi_link_ap_ip_str(ip, sizeof(ip));
    snprintf(line, sizeof(line), "%s", ip);
    oled_line(row++, line);
    snprintf(line, sizeof(line), "%d devices", wifi_link_ap_station_count());
    oled_line(row++, line);
  } else
    oled_line(row++, "Sleeping");

  oled_section_offset += section_gap;
  row = oled_section(row, "Forwarding");
  if (!radio.available || radio.error)
    snprintf(line, sizeof(line), "Radio error");
  else if (!radio.receiving)
    snprintf(line, sizeof(line), "Radio off");
  else
    snprintf(line, sizeof(line), "%luMHz %u seen",
             (unsigned long)(radio.frequency_hz / 1000000), (unsigned)count);
  oled_line(row++, line);
  snprintf(line, sizeof(line), "RL: %d 24h",
           upload_stats_count_last_24h(UPLOAD_TARGET_RL));
  oled_line(row++, line);
  if (cfg->wu_map_count) {
    snprintf(line, sizeof(line), "WU: %d 24h",
             upload_stats_count_last_24h(UPLOAD_TARGET_WU));
    oled_line(row++, line);
  }
  if (row < end) oled_line(row++, "Device rcvd/sent");
  // As on the C6, recently uploading weather stations sort before idle clients.
  for (int i = 1; i < nc; i++) {
    ap_client_t current = clients[i];
    int j = i;
    while (j > 0 && clients[j - 1].last_upload_us < current.last_upload_us) {
      clients[j] = clients[j - 1];
      j--;
    }
    clients[j] = current;
  }
  for (int i = 0; i < nc && row < end; i++) {
    const ap_client_t *client = &clients[i];
    if (!client->last_upload_us && !client->connected) continue;
    char fallback[16], counters[24];
    snprintf(fallback, sizeof(fallback), IPSTR, IP2STR(&client->ip));
    const char *name = *client->name       ? client->name
                       : *client->hostname ? client->hostname
                                           : fallback;
    snprintf(counters, sizeof(counters), "%lu/%lu",
             (unsigned long)client->rx_count, (unsigned long)client->fwd_count);
    int width = OLED_COLUMNS - (int)strlen(counters) - 1;
    snprintf(line, sizeof(line), "%.*s %s", width > 0 ? width : 0, name,
             counters);
    oled_line(row++, line);
  }
  for (size_t i = 0; i < count && row < end; i++) {
    const weather_packet_t *p = &sensors[i].reading.packet;
    snprintf(line, sizeof(line), "%s %u%c: %lurx",
             p->model == WEATHER_LACROSSE_TX5U ? "TX5U" : "Iris", p->id,
             p->channel ? p->channel : ' ', (unsigned long)sensors[i].packets);
    oled_line(row++, line);
  }
  if (pending) {
    snprintf(line, sizeof(line), "Retry queue: %d", pending);
    oled_line(end++, line);
  }
  if (update) {
    char latest[24];
    ota_update_latest(latest, sizeof(latest));
    snprintf(line, sizeof(line), "New FW: v%.12s", latest);
    oled_line(end, line);
  }
}
#endif

static void oled_draw(uint32_t held) {
  oled_section_offset = 0;
  oled_row_limit = OLED_ROWS;
  // Undo the side-strip rotation of the approved logo artwork.
  for (int y = 0; y < OLED_LOGO_H; y++)
    for (int x = 0; x < OLED_LOGO_W; x++)
      if (oled_logo_pages[(y / 8) * OLED_LOGO_W + x] & (1U << (y % 8)))
        display_fill_rect(OLED_LOGO_H - 1 - y, x, 1, 1, COLOR_WHITE);
  char version[OLED_COLUMNS + 1];
  snprintf(version, sizeof(version), "v%.*s", OLED_COLUMNS - 1, esp_app_get_description()->version);
  int version_x = DISPLAY_W - OLED_VERSION_RIGHT - (int)strlen(version) * GLYPH;
  display_text(version_x, 15, 1, COLOR_WHITE, version);
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
#if BOARD_DISPLAY_FONT == BOARD_FONT_4X6
    oled_status_dense();
#else
    int row = 0;
    if (wifi_link_sta_has_ip())
      wifi_link_sta_ip_str(line, sizeof(line));
    else
      snprintf(line, sizeof(line), "Wi-Fi offline");
    oled_line(row++, line);

    radio_status_t status;
    radio_status(&status);
    radio_sensor_t sensors[RADIO_SENSORS_MAX];
    size_t count = radio_sensors(sensors, RADIO_SENSORS_MAX);
    if (!status.available || status.error)
      snprintf(line, sizeof(line), "Radio error");
    else if (!status.receiving)
      snprintf(line, sizeof(line), "Radio off");
    else
      snprintf(line, sizeof(line), "%luMHz: %u seen",
               (unsigned long)(status.frequency_hz / 1000000), (unsigned)count);
    oled_line(row++, line);

    int pending = forwarder_pending_count();
    bool update = ota_update_available();
    int last = OLED_ROWS - ((pending || update) ? 1 : 0);
    if (row < last) {
      snprintf(line, sizeof(line), "RL 24h: %d",
               upload_stats_count_last_24h(UPLOAD_TARGET_RL));
      oled_line(row++, line);
    }
    if (cfg->wu_map_count && row < last) {
      snprintf(line, sizeof(line), "WU 24h: %d",
               upload_stats_count_last_24h(UPLOAD_TARGET_WU));
      oled_line(row++, line);
    }
    if (wifi_link_ap_enabled() && row < last) {
      snprintf(line, sizeof(line), "WiFi: %d clients",
               wifi_link_ap_station_count());
      oled_line(row++, line);
    }
    if (status.receiving && count && row < last) {
      int64_t newest = 0;
      for (size_t i = 0; i < count; i++)
        if (sensors[i].reading.received_us > newest)
          newest = sensors[i].reading.received_us;
      int64_t seconds = (esp_timer_get_time() - newest) / 1000000;
      if (seconds < 60)
        snprintf(line, sizeof(line), "Last RX: %llds", (long long)seconds);
      else if (seconds < 3600)
        snprintf(line, sizeof(line), "Last RX: %lldm",
                 (long long)(seconds / 60));
      else
        snprintf(line, sizeof(line), "Last RX: %lldh",
                 (long long)(seconds / 3600));
      oled_line(row++, line);
    }
    if (pending) {
      snprintf(line, sizeof(line), "Queued: %d", pending);
      oled_line(OLED_ROWS - 1, line);
    } else if (update)
      oled_line(OLED_ROWS - 1, "FW update ready");
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
    if (frame->provisioned)
      screen_status_draw();
    else
      screen_setup_draw();
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
  // Includes transient radio inventory (OLED) or Wi-Fi station refresh
  // (LCD). The persistent display snapshot and render buffer are off-stack.
  xTaskCreate(ui_task, "ui", 6144, NULL, 3, NULL);
}
