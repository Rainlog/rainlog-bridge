// Rainlog Bridge board identities, display geometry and GPIO assignments.
//
// Single source of truth for GPIO assignments (DRY). Values from the official
// Waveshare and LILYGO demos (see README.md / ./fetch-demos.sh).

#pragma once

#include "sdkconfig.h"

#define BOARD_FONT_4X6 406
#define BOARD_FONT_6X10 610
#define BOARD_FONT_8X13 813

#if CONFIG_IDF_TARGET_ESP32
// LILYGO T3 LoRa32 V1.6.1, 433 MHz SX1278 variant.
#define BOARD_ID "lilygo-t3-v1.6.1-sx1278"
#define BOARD_DISPLAY_SSD1306 1
#define BOARD_DISPLAY_NATIVE_W 128
#define BOARD_DISPLAY_NATIVE_H 64
// Portrait coordinates, rotated clockwise into the SSD1306 page buffer.
#define BOARD_DISPLAY_W 64
#define BOARD_DISPLAY_H 128
#ifndef BOARD_DISPLAY_FONT
#define BOARD_DISPLAY_FONT BOARD_FONT_4X6
#endif
#define BOARD_I2C_SDA_GPIO 21
#define BOARD_I2C_SCL_GPIO 22
#define BOARD_OLED_ADDRESS 0x3C
#define BOARD_RADIO_SCLK_GPIO 5
#define BOARD_RADIO_MISO_GPIO 19
#define BOARD_RADIO_MOSI_GPIO 27
#define BOARD_RADIO_CS_GPIO 18
#define BOARD_RADIO_RST_GPIO 23
#define BOARD_RADIO_DIO0_GPIO 26
#define BOARD_RADIO_DIO1_GPIO 33
#define BOARD_RADIO_DIO2_GPIO 32
#define BOARD_STATUS_LED_GPIO 25
// Optional external button to ground. This board only has an onboard RST button.
#define BOARD_BOOT_BUTTON_GPIO 0
#elif CONFIG_IDF_TARGET_ESP32C6
#define BOARD_DISPLAY_SSD1306 0
#define BOARD_DISPLAY_W 172
#define BOARD_DISPLAY_H 320
#ifndef BOARD_DISPLAY_FONT
#define BOARD_DISPLAY_FONT BOARD_FONT_8X13
#endif
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
// this non-touch board; a press wakes the display and a sustained hold
// starts the factory-reset countdown.
#define BOARD_BOOT_BUTTON_GPIO 9

#else
#error "Unsupported Rainlog Bridge target: select esp32c6 or esp32"
#endif
