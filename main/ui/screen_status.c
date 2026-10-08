#include "screen_status.h"

#include <stdio.h>
#include <string.h>

#include "ap_clients.h"
#include "config_store.h"
#include "display.h"
#include "esp_timer.h"
#include "forwarder.h"
#include "ota_update.h"
#include "ui_common.h"
#include "upload_stats.h"
#include "wifi_link.h"

// Vertical layout (172x320 portrait), below the shared header: three labeled
// sections with bold underlined headers - Home Wi-Fi (the LAN/uplink side),
// Bridge Wi-Fi (the SoftAP side), Forwarding (delivery status + per-target
// counts: the Rainlog row highlighted bold/blue, the WU relay row plain grey).
#define X_TEXT 6
#define ROW_H 15
#define SECTION_GAP 8
// Latest y a row may start at and still fit on the panel; rows past it are
// simply dropped (with the bottom cells gone everything fits in practice).
#define Y_ROW_LAST 305

// Parked rows: the "Last: <result> <ago>s" + "Uptime:" pair, probably not
// worth their space (the RL counts + per-client list cover the same
// questions). Flip to 1 to bring them back (they re-count themselves in
// `trailing`).
#define SHOW_LAST_AND_UPTIME 0

// Section tint: the wordmark's brand blue #62b2c2 (see gen-header.py).
#define SECTION_TINT display_rgb(0x62, 0xb2, 0xc2)

// Section header: bold caps label with a rule underlining the full row, both
// in the section tint. Returns the y of the section's first content row.
static int draw_section(int y, const char *label) {
  display_text_bold(X_TEXT, y, 1, SECTION_TINT, label);
  display_fill_rect(X_TEXT, y + GLYPH_H + 1, DISPLAY_W - 2 * X_TEXT, 2,
                    SECTION_TINT);
  return y + GLYPH_H + 7;
}

// Map RSSI (dBm) to a 0..4 signal level.
static int rssi_level(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

// Draw a 4-bar signal indicator with its bottom at y_base; bars rise to the
// right. Active bars colored by strength, the rest dim.
static void draw_signal_bars(int x, int y_base, int level) {
  display_color_t on =
      level >= 3 ? COLOR_GREEN : (level == 2 ? COLOR_AMBER : COLOR_RED);
  display_color_t off = display_rgb(70, 70, 70);
  for (int i = 0; i < 4; i++) {
    int h = (i + 1) * 3;
    display_fill_rect(x + i * 4, y_base - h, 3, h, i < level ? on : off);
  }
}

#if SHOW_LAST_AND_UPTIME
static display_color_t result_color(const char *r) {
  if (strcmp(r, "OK") == 0) {
    return COLOR_GREEN;
  }
  if (strcmp(r, "FAIL") == 0) {
    return COLOR_RED;
  }
  if (strcmp(r, "REJECT") == 0) {
    return COLOR_AMBER;
  }
  return COLOR_GREY;
}
#endif  // SHOW_LAST_AND_UPTIME

// Capture dynamic values once so all strips display the same frame.
static struct {
  char ssid[33], sta_ip[16], ap_ip[16], latest[24];
  int rssi, station_count, rl24, wu24, pending, client_count;
  uint32_t rl_total, wu_total;
  bool have_net, up, update_available;
  ap_client_t clients[AP_CLIENTS_MAX];
#if SHOW_LAST_AND_UPTIME
  char result[16];
  int64_t seconds_since_upload, uptime_s;
#endif
} frame;

void screen_status_prepare(void) {
  frame.have_net = wifi_link_sta_ap_info(frame.ssid, sizeof(frame.ssid), &frame.rssi);
  frame.up = wifi_link_sta_has_ip();
  wifi_link_sta_ip_str(frame.sta_ip, sizeof(frame.sta_ip));
  wifi_link_ap_ip_str(frame.ap_ip, sizeof(frame.ap_ip));
  frame.station_count = wifi_link_ap_station_count();
  frame.rl24 = upload_stats_count_last_24h(UPLOAD_TARGET_RL);
  frame.wu24 = upload_stats_count_last_24h(UPLOAD_TARGET_WU);
  frame.rl_total = upload_stats_total(UPLOAD_TARGET_RL);
  frame.wu_total = upload_stats_total(UPLOAD_TARGET_WU);
  frame.pending = forwarder_pending_count();
  frame.update_available = ota_update_available();
  ota_update_latest(frame.latest, sizeof(frame.latest));
  frame.client_count = ap_clients_snapshot(frame.clients, AP_CLIENTS_MAX);
  for (int i = 1; i < frame.client_count; i++) {
    ap_client_t c = frame.clients[i];
    int j = i;
    for (; j > 0 && frame.clients[j - 1].last_upload_us < c.last_upload_us; j--)
      frame.clients[j] = frame.clients[j - 1];
    frame.clients[j] = c;
  }
#if SHOW_LAST_AND_UPTIME
  snprintf(frame.result, sizeof(frame.result), "%s", forwarder_last_result());
  frame.seconds_since_upload = forwarder_seconds_since_upload();
  frame.uptime_s = esp_timer_get_time() / 1000000;
#endif
}

void screen_status_draw(void) {
  const bridge_config_t *cfg = config_get();
  char buf[48];
  int y = Y_CONTENT_TOP;

  // ---- Home Wi-Fi (the LAN side) -------------------------------------------
  y = draw_section(y, "Home Wi-Fi");

  // Network name, colored by uplink state (green = up, amber = connecting),
  // with signal-strength bars on the right. Truncated so it never runs into
  // the bars; falls back to the configured SSID while not associated.
  snprintf(buf, sizeof(buf), "%.17s", frame.have_net ? frame.ssid : cfg->sta_ssid);
  display_text(X_TEXT, y, 1, frame.up ? COLOR_GREEN : COLOR_AMBER, buf);
  if (frame.have_net) draw_signal_bars(DISPLAY_W - 20, y + 11, rssi_level(frame.rssi));
  y += ROW_H;

  // The bridge's address on the home LAN ("--" while the uplink is down);
  // where to reach the setup page from that side.
  display_text(X_TEXT, y, 1, COLOR_GREY, frame.sta_ip);
  y += ROW_H + SECTION_GAP;

  // ---- Bridge Wi-Fi (the SoftAP side) ---------------------------------------
  y = draw_section(y, "Bridge Wi-Fi");

  display_text(X_TEXT, y, 1, COLOR_GREEN, cfg->ap_ssid);
  y += ROW_H;

  // The setup-page address on the bridge's own network, plus how many devices
  // (weather consoles / phones) are joined, right-aligned.
  display_text(X_TEXT, y, 1, COLOR_GREY, frame.ap_ip);
  int n = frame.station_count;
  snprintf(buf, sizeof(buf), "%d device%s", n, n == 1 ? "" : "s");
  display_text(DISPLAY_W - X_TEXT - (int)strlen(buf) * GLYPH, y, 1, COLOR_WHITE,
               buf);
  y += ROW_H + SECTION_GAP;

  // ---- Forwarding -----------------------------------------------------------
  y = draw_section(y, "Forwarding");

  // Rainlog successes: the headline counts, highlighted bold; blue, or red
  // when nothing has come in for a day (likely a problem).
  display_color_t blue = display_rgb(96, 176, 255);
  int c24 = frame.rl24;
  snprintf(buf, sizeof(buf), "RL: %d 24h, %lu total", c24,
           (unsigned long)frame.rl_total);
  display_text_bold(X_TEXT, y, 1, c24 > 0 ? blue : COLOR_RED, buf);
  y += ROW_H;

  // WU relay successes, plain grey, only when relaying is configured.
  if (cfg->wu_map_count > 0 && y <= Y_ROW_LAST) {
    snprintf(buf, sizeof(buf), "WU: %d 24h, %lu total",
             frame.wu24,
             (unsigned long)frame.wu_total);
    display_text(X_TEXT, y, 1, COLOR_GREY, buf);
    y += ROW_H;
  }

  // Devices on the bridge's Wi-Fi (user-given name, else DHCP hostname, else
  // IP), each with its received / forwarded-to-Rainlog counts: every device
  // that has uploaded this boot (newest first) plus any currently connected
  // one that hasn't (0/0) - as many as fit while leaving room for the
  // trailing rows below.
  int pending = frame.pending;
  int trailing = (pending > 0 ? 1 : 0) + (frame.update_available ? 1 : 0) +
                 2 * SHOW_LAST_AND_UPTIME;
  int y_clients_last = Y_ROW_LAST - trailing * ROW_H;
  const ap_client_t *clients = frame.clients;
  int nc = frame.client_count;
  bool any_listed = false;
  for (int i = 0; i < nc; i++) {
    if (clients[i].last_upload_us != 0 || clients[i].connected) {
      any_listed = true;
      break;
    }
  }
  // Column header so the per-client counts read as a table: bold, a notch
  // dimmer than the data rows so it still reads as a heading.
  if (any_listed && y <= y_clients_last) {
    display_color_t hdr = display_rgb(160, 160, 160);
    display_text_bold(X_TEXT, y, 1, hdr, "device");
    const char *cols = "rcvd/sent";
    display_text_bold(DISPLAY_W - X_TEXT - (int)strlen(cols) * GLYPH, y, 1, hdr,
                      cols);
    y += ROW_H;
  }
  for (int i = 0; i < nc && y <= y_clients_last; i++) {
    if (clients[i].last_upload_us == 0 && !clients[i].connected) {
      continue;  // gone and never uploaded: not worth a row
    }
    char who[64];
    if (clients[i].name[0] != '\0') {
      snprintf(who, sizeof(who), "%s", clients[i].name);
    } else if (clients[i].hostname[0] != '\0') {
      snprintf(who, sizeof(who), "%s", clients[i].hostname);
    } else {
      snprintf(who, sizeof(who), IPSTR, IP2STR(&clients[i].ip));
    }
    // received/forwarded counts right-aligned; the name fills what's left of
    // the 20-char row (minus a separating space).
    snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)clients[i].rx_count,
             (unsigned long)clients[i].fwd_count);
    int counts_len = (int)strlen(buf);
    display_text(DISPLAY_W - X_TEXT - counts_len * GLYPH, y, 1, COLOR_WHITE,
                 buf);
    snprintf(buf, sizeof(buf), "%.*s", 20 - counts_len - 1, who);
    display_text(X_TEXT, y, 1, COLOR_GREY, buf);
    y += ROW_H;
  }

  // Uploads buffered for retry, only when there are any.
  if (pending > 0 && y <= Y_ROW_LAST) {
    snprintf(buf, sizeof(buf), "Retry queue: %d", pending);
    display_text(X_TEXT, y, 1, COLOR_RED, buf);
    y += ROW_H;
  }

#if SHOW_LAST_AND_UPTIME
  // Last forward result + how long ago.
  const char *res = frame.result;
  int64_t ago = frame.seconds_since_upload;
  if (ago >= 0) {
    snprintf(buf, sizeof(buf), "Last: %s  %llds", res, (long long)ago);
  } else {
    snprintf(buf, sizeof(buf), "Last: none yet");  // nothing forwarded yet
  }
  if (y <= Y_ROW_LAST) {
    display_text(X_TEXT, y, 1, result_color(res), buf);
    y += ROW_H;
  }

  // Uptime since last boot.
  if (y <= Y_ROW_LAST) {
    int64_t up_s = frame.uptime_s;
    snprintf(buf, sizeof(buf), "Uptime: %dh %dm", (int)(up_s / 3600),
             (int)((up_s % 3600) / 60));
    display_text(X_TEXT, y, 1, COLOR_GREY, buf);
    y += ROW_H;
  }
#endif  // SHOW_LAST_AND_UPTIME

  // Newer-firmware-available notice (apply it from the setup page). Only when
  // a check has found a newer version.
  if (frame.update_available && y <= Y_ROW_LAST) {
    snprintf(buf, sizeof(buf), "New FW: v%.12s", frame.latest);
    display_text(X_TEXT, y, 1, COLOR_AMBER, buf);
  }
}
