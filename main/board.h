// Rainlog Wireless Bridge - board pin map (Waveshare ESP32-C6-LCD-1.47).
//
// Single source of truth for GPIO assignments (DRY). Values from the official
// Waveshare demo drivers (see README.md / ./fetch-demo.sh). LCD/SD pins are
// listed for when the LCD UI lands.

#pragma once

// Board identity, published in the OTA manifest ("board"), checked by the
// firmware so a device never applies another board's image, and used to derive
// the manifest filename (see CFG_OTA_MANIFEST_PATH). It encodes the chip's
// flash variant because that fixes the partition layout: this build is for the
// ESP32-C6FH8 (8MB) with the 8MB table; an ESP32-C6FH4 (4MB) build gets its
// own id (`...-c6fh4`), 4MB table (`partitions-c6fh4.csv`), and image stream.
#define BOARD_ID "esp32-c6fh8-lcd-1.47"

// RGB status LED (single WS2812 / NeoPixel).
#define BOARD_RGB_LED_GPIO 8
#define BOARD_RGB_LED_COUNT 1

// Shared SPI bus (LCD + SD card).
#define BOARD_SPI_SCLK_GPIO 7
#define BOARD_SPI_MOSI_GPIO 6
#define BOARD_SPI_MISO_GPIO 5

// ST7789 LCD (172x320).
#define BOARD_LCD_CS_GPIO 14
#define BOARD_LCD_DC_GPIO 15
#define BOARD_LCD_RST_GPIO 21
#define BOARD_LCD_BL_GPIO 22

// MicroSD card.
#define BOARD_SD_CS_GPIO 4

// BOOT button (active low, has an external pull-up). The only user input on
// this non-touch board; used as the interaction signal (wake the backlight) and
// later for hold-to-provision.
#define BOARD_BOOT_BUTTON_GPIO 9
