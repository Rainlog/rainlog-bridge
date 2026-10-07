// Panel transport shared by the RGB565 renderer. Flush completes before
// returning, so the renderer can immediately reuse its single framebuffer.
#pragma once
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_panel_init(void);
esp_err_t display_panel_flush(const uint16_t *pixels);
esp_err_t display_panel_brightness(uint8_t percent);
