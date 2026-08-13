#include "screen_setup.h"

#include <stdio.h>

#include "config_store.h"
#include "display.h"
#include "ui_common.h"
#include "wifi_link.h"

// Indent for the values listed under each numbered step.
#define X_STEP 6
#define X_VALUE 20

void screen_setup_draw(void) {
  const bridge_config_t *cfg = config_get();
  char buf[80];  // "pass: " + up to a 64-char password

  int y = Y_CONTENT_TOP;

  ui_center_text(y, 1, COLOR_AMBER, "NOT CONFIGURED");
  y += 32;

  // Step 1: join the bridge's own Wi-Fi network (SSID + password together).
  display_text(X_STEP, y, 1, COLOR_WHITE, "1. Join this Wi-Fi:");
  y += 18;
  display_text(X_VALUE, y, 1, COLOR_GREEN, cfg->ap_ssid);
  y += 16;
  snprintf(buf, sizeof(buf), "pass: %s", cfg->ap_pass);
  display_text(X_VALUE, y, 1, COLOR_GREEN, buf);
  y += 32;

  // Step 2: open the configurator in a browser at the bridge's IP.
  display_text(X_STEP, y, 1, COLOR_WHITE, "2. Open in a browser:");
  y += 18;
  char ip[16];
  wifi_link_ap_ip_str(ip, sizeof(ip));
  display_text(X_VALUE, y, 1, COLOR_BLUE, ip);
  y += 32;

  // Live feedback so the user can confirm their device actually joined.
  int n = wifi_link_ap_station_count();
  snprintf(buf, sizeof(buf), "Connected: %d device%s", n, n == 1 ? "" : "s");
  display_text(X_STEP, y, 1, COLOR_GREY, buf);
}
