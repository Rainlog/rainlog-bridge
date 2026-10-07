#include "ui_common.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_app_desc.h"
#include "header_logo.h"

void ui_center_text(int y, int scale, display_color_t color, const char *s) {
  int w = (int)strlen(s) * GLYPH * scale;
  int x = (DISPLAY_W - w) / 2;
  if (x < 0) {
    x = 0;
  }
  display_text(x, y, scale, color, s);
}

void ui_draw_header(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  char buf[24];
  // Pre-rendered, antialiased raindrop + "Rainlog / Bridge" wordmark (one
  // pixmap; see tools/gen-header.py). Full panel width, so x=0.
  display_blit_rgba(0, Y_HEADER, HEADER_LOGO_W, HEADER_LOGO_H,
                    header_logo_rgba);
  snprintf(buf, sizeof(buf), "v%.12s", app->version);
  // Version label under the wordmark (which starts at x=53 in the pixmap),
  // tucked up 4px toward it. Grey (150) nudged 40% toward white -> ~192.
  display_text(53, Y_HEADER + HEADER_LOGO_H - 4, 1, display_rgb(192, 192, 192),
               buf);
}
