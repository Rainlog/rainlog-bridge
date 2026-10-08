// Exercise actual status drawing with deterministic network/radio snapshots.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "display.h"
#include "font4x6.h"
#include "radio/radio.h"
#include "stats/upload_stats.h"
#define AP_CLIENTS_MAX 8
#define Y_CONTENT_TOP 82
#define RESET_ARM_MS 1500
#define RESET_HOLD_MS 11000
typedef struct {
  bool connected;
  int64_t last_upload_us;
  uint32_t gauge_id;
  unsigned err_flags;
  uint32_t rx_count, fwd_count;
  uint32_t ip;
  char name[33], hostname[64];
  const char *vendor;
} ap_client_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 10u, 41u, 0u, 2u
typedef struct {
  char version[24];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void) {
  static const esp_app_desc_t app = {.version = "1.0.0"};
  return &app;
}
void ota_update_latest(char *out, size_t size) { snprintf(out, size, "1.0.1"); }
uint32_t upload_stats_total(upload_target_t target) {
  return target == UPLOAD_TARGET_RL ? 3456 : 7890;
}
static bridge_config_t cfg;
static bool up = true, ap = true, update, provisioned = true;
static int pending, row_count;
static char result[16] = "OK";
static struct {
  int x, y;
  char text[80];
} rows[32];
static unsigned char pixels[DISPLAY_W * DISPLAY_H];
static int64_t now = 90000000;
const bridge_config_t *config_get(void) { return &cfg; }
bool config_is_provisioned(void) { return provisioned; }
int64_t esp_timer_get_time(void) { return now; }
bool wifi_link_sta_has_ip(void) { return up; }
bool wifi_link_ap_enabled(void) { return ap; }
bool wifi_link_sta_ap_info(char *ssid, size_t size, int *rssi) {
  snprintf(ssid, size, "A very long home network name");
  *rssi = -60;
  return up;
}
bool wifi_link_sta_ip_str(char *out, size_t size) {
  snprintf(out, size, "%s", up ? "192.168.100.123" : "--");
  return up;
}
void wifi_link_ap_ip_str(char *out, size_t size) {
  snprintf(out, size, "10.41.0.1");
}
int wifi_link_ap_station_count(void) { return 2; }
int upload_stats_count_last_24h(upload_target_t target) {
  return target == UPLOAD_TARGET_RL ? 12 : 34;
}
int forwarder_pending_count(void) { return pending; }
const char *forwarder_last_result(void) { return result; }
bool ota_update_available(void) { return update; }
int ap_clients_snapshot(ap_client_t *out, int max) {
  assert(max >= 2);
  out[0] = (ap_client_t){
      .connected = true, .vendor = "Phone", .name = "Setup phone"};
  out[1] = (ap_client_t){.connected = true,
                         .last_upload_us = now - 12000000,
                         .gauge_id = 123,
                         .vendor = "Weather station",
                         .name = "Back yard weather station"};
  return 2;
}
void radio_status(radio_status_t *out) {
  *out = (radio_status_t){
      .available = true, .receiving = true, .frequency_hz = 433920000};
}
size_t radio_sensors(radio_sensor_t *out, size_t max) {
  assert(max >= 1);
  *out = (radio_sensor_t){.reading.received_us = now - 12000000};
  return 1;
}
display_color_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return !!(r || g || b);
}
void display_text(int x, int y, int scale, display_color_t color,
                  const char *text) {
  (void)color;
  assert(scale == 1 && row_count < 32);
  assert(y >= 0 && y + GLYPH_H <= DISPLAY_H);
  if (y == 15)
    assert(x + (int)strlen(text) * GLYPH == DISPLAY_W - 7);
  else
    assert(x == 2);
  assert(x + (int)strlen(text) * GLYPH <=
         DISPLAY_W - 2 + (BOARD_DISPLAY_FONT == BOARD_FONT_6X10 ? 1 : 0));
  for (int i = 0; i < row_count; i++) assert(y >= rows[i].y + GLYPH_H);
#if BOARD_DISPLAY_FONT == BOARD_FONT_4X6
  for (const unsigned char *p = (const unsigned char *)text; *p;
       p++, x += GLYPH) {
    for (int gy = 0; gy < GLYPH_H; gy++)
      for (int gx = 0; gx < GLYPH; gx++) {
        if (x + gx < DISPLAY_W && (font4x6[*p][gy] & (1 << gx)))
          pixels[(y + gy) * DISPLAY_W + x + gx] = 255;
      }
  }
#endif
  rows[row_count].x = x;
  rows[row_count].y = y;
  snprintf(rows[row_count++].text, sizeof(rows[0].text), "%s", text);
}
void display_text_bold(int x, int y, int scale, display_color_t color,
                       const char *text) {
  display_text(x, y, scale, color, text);
}
void display_fill_rect(int x, int y, int w, int h, display_color_t color) {
  assert(x >= 0 && y >= 0 && x + w <= DISPLAY_W && y + h <= DISPLAY_H);
  for (int py = y; py < y + h; py++)
    for (int px = x; px < x + w; px++) pixels[py * DISPLAY_W + px] = color ? 255 : 0;
}
void display_blit_mono(int x, int y, int w, int h, const uint8_t *data) {
  for (int py = 0; py < h; py++)
    for (int px = 0; px < w; px++)
      if (data[(py / 8) * w + px] & (1 << (py % 8)))
        pixels[(y + py) * DISPLAY_W + x + px] = 255;
}
#include "status_under_test.c"
static bool has(const char *text) {
  for (int i = 0; i < row_count; i++)
    if (!strcmp(rows[i].text, text)) return true;
  return false;
}
static void draw(void) {
  row_count = 0;
  memset(pixels, 0, sizeof(pixels));
#if BOARD_DISPLAY_SSD1306
  oled_draw(0);
#else
  screen_status_prepare();
  screen_status_draw();
#endif
}
int main(int argc, char **argv) {
  strcpy(cfg.ap_ssid, "RainlogBridge-long-network-name");
  cfg.wu_map_count = 1;
#if BOARD_DISPLAY_SSD1306
  cfg.radio_map_count = 1;
  ap = false;
#endif
  draw();
  if (argc > 1) {
    FILE *file = fopen(argv[1], "wb");
    assert(file);
    fprintf(file, "P5\n%d %d\n255\n", DISPLAY_W, DISPLAY_H);
    assert(fwrite(pixels, 1, sizeof(pixels), file) == sizeof(pixels));
    fclose(file);
  }
#if BOARD_DISPLAY_FONT == BOARD_FONT_4X6
  assert(has("192.168.100.123"));
  assert(has("433MHz 1 seen") && has("v1.0.0"));
  assert(has("Home Wi-Fi") && has("Bridge Wi-Fi") && has("Forwarding") &&
         has("RL: 12 24h") && has("3456 total") && has("WU: 34 24h") && has("7890 total"));
  for (int i = 1; i < row_count; i++)
    if (!strcmp(rows[i].text, "Bridge Wi-Fi") ||
        !strcmp(rows[i].text, "Forwarding"))
      assert(rows[i].y - rows[i - 1].y == GLYPH_H + 1 + 3);
  pending = 3;
  update = true;
  draw();
  assert(has("Retry queue: 3") && has("New FW: v1.0.1"));
  up = false;
  draw();
  assert(has("Wi-Fi offline"));
#else
  assert(has("192.168.100.123"));
#if BOARD_DISPLAY_SSD1306
  assert(has("433MHz: 1 seen") && has("Last RX: 12s"));
  assert(has("RL 24h: 12") && has("WU 24h: 34"));
  assert(!has("Rainlog Bridge") && !has("OK"));
#else
  assert(has("Rainlog: 12") && has("WU: 34"));
  assert(!has("Setup phone"));
#endif
  pending = 3;
  update = true;
  strcpy(result, "FAIL");
  draw();
#if BOARD_DISPLAY_SSD1306
  assert(has("Queued: 3"));
#else
  assert(has("Queued: 3 uploads") && has("Rainlog: FAIL") &&
         has("FW update available"));
#endif
  pending = 0;
  draw();
  assert(
      has(BOARD_DISPLAY_SSD1306 ? "FW update ready" : "FW update available"));
  update = false;
  up = false;
  ap = false;
  draw();
  assert(has(BOARD_DISPLAY_SSD1306 ? "Wi-Fi offline" : "Connecting..."));
#if !BOARD_DISPLAY_SSD1306
  assert(has("Wi-Fi capture off"));
#else
  cfg.radio_map_count = 0;
  cfg.wu_map_count = 0;
  draw();
  assert(has("No uploaders") && !has("RL 24h: 12") && !has("WU 24h: 34"));
  cfg.wifi_interception_enabled = 1;
  ap = true;
  draw();
  assert(has("RL 24h: 12") && has("WiFi: 2 clients"));
#endif
#endif
  puts("Status content, clipping, non-overlap and warning priority passed");
}
