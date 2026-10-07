#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "weather_decode.h"
#define RADIO_HISTORY 16
// Retain distinct sensors independently of packet repeats, within ESP32 RAM.
#define RADIO_SENSORS_MAX 16
typedef struct {
  bool available, receiving;
  uint8_t chip_version, ook_floor;
  uint32_t frequency_hz, bandwidth_hz, bursts, durations, packets, dropped;
  float rssi_dbm;
  esp_err_t error;
} radio_status_t;
typedef struct {
  weather_packet_t packet;
  int64_t received_us;
  float rssi_dbm;
} radio_reading_t;
typedef struct {
  radio_reading_t reading;
  uint32_t packets;
} radio_sensor_t;
size_t radio_sensors(radio_sensor_t *out, size_t max);
// Initialize SX1278 and begin receive-only OOK operation when compiled in.
void radio_start(void);
void radio_status(radio_status_t *out);
size_t radio_readings(radio_reading_t *out, size_t max);
// Receive controls: no transmit API. Settings are temporary until reboot.
esp_err_t radio_receive_enable(bool enable);
esp_err_t radio_tune(uint32_t frequency_hz, uint32_t bandwidth_hz,
                     uint8_t ook_floor);
