#pragma once

// Physical radio band, supplied by the board build. A disabled receiver has
// no enabled protocols even when its board has a radio.
#ifndef RAINLOG_RADIO
#error "RAINLOG_RADIO must be supplied by the build"
#endif
#ifndef RAINLOG_RADIO_MHZ
#error "RAINLOG_RADIO_MHZ must be supplied by the board build"
#endif

#define WEATHER_PROTOCOL_LACROSSE_TX5U \
  (RAINLOG_RADIO && RAINLOG_RADIO_MHZ == 433)
#define WEATHER_PROTOCOL_ACURITE_IRIS \
  (RAINLOG_RADIO && RAINLOG_RADIO_MHZ == 433)
// Reserved for the Ambient WH65B/WS69 FSK decoder, not yet implemented.
#define WEATHER_PROTOCOL_AMBIENT_ARRAY \
  (RAINLOG_RADIO && RAINLOG_RADIO_MHZ == 915)
#define WEATHER_PROTOCOL_OOK \
  (WEATHER_PROTOCOL_LACROSSE_TX5U || WEATHER_PROTOCOL_ACURITE_IRIS)
