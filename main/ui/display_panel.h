// Panel transport using native RGB565 pixels or SSD1306 page bytes. Flush
// completes before returning, so the renderer can immediately reuse its single
// render buffer.
#pragma once
#include <stdint.h>

#include "display.h"
#include "esp_err.h"

esp_err_t display_panel_init(void);
esp_err_t display_panel_flush(const display_color_t *pixels, int y, int rows);
esp_err_t display_panel_brightness(uint8_t percent);
