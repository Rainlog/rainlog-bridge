# Display controller datasheets

Downloaded on 2026-10-07. These are manufacturer datasheets hosted by display suppliers; the PDFs retain their original copyright notices.

| Local file | Controller | Revision | Source |
|---|---|---|---|
| `ST7789VW.pdf` | Sitronix ST7789VW | 1.0, September 2017 | [Waveshare](https://files.waveshare.com/upload/a/ae/ST7789_Datasheet.pdf) |
| `ST7789T3.pdf` | Sitronix ST7789T3 | 1.0, May 2022 | [Waveshare](https://files.waveshare.com/wiki/2.8inch-Capacitive-Touch-LCD/ST7789T3_SPEC_V1.0.pdf) |
| `SSD1306.pdf` | Solomon Systech SSD1306 | 1.1, April 2008, with appended application note | [Adafruit](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf) |

The Waveshare ESP32-C6-LCD-1.47 documentation identifies its controller as ST7789 without a suffix. Keep both ST7789 references for comparison; the local driver's former `st7789t` name does not establish the exact silicon variant. [Board specifications](https://docs.waveshare.com/ESP32-C6-LCD-1.47).

The LILYGO T3 LoRa32 V1.6.1 uses an SSD1306 OLED at I2C address `0x3C`, with SDA on GPIO21 and SCL on GPIO22. [LILYGO board documentation](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t3_v161_sx1276/t3_v161_sx1276_hw.md).
