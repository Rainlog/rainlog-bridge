#include "radio.h"

#include <string.h>

#include "board.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if RAINLOG_RADIO
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/spi_master.h"
#include "sx127x.h"
#include "sx127x_registers.h"

static const char *TAG = "radio";
static sx127x chip;
static spi_device_handle_t spi;
static rmt_channel_handle_t rx_channel;
static rmt_symbol_word_t symbols[256];
static QueueHandle_t events;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_status_t status;
static radio_reading_t history[RADIO_HISTORY];
static unsigned history_count, history_next;
static radio_sensor_t seen_sensors[RADIO_SENSORS_MAX];
static unsigned seen_count;
static weather_decoder_t decoder;
static struct {
  bool high;
  uint32_t duration;
} pulses[512];
static void record_duration(bool high, uint32_t duration);
static void filter_capture(size_t count) {
  size_t used = 0;
  for (size_t i = 0; i < count; i++) {
    for (unsigned half = 0; half < 2; half++) {
      bool high = half ? symbols[i].level1 : symbols[i].level0;
      uint32_t duration = half ? symbols[i].duration1 : symbols[i].duration0;
      if (!duration) continue;
      pulses[used].high = high;
      pulses[used++].duration = duration;
      while (used >= 3 && pulses[used - 2].duration < 80 &&
             pulses[used - 3].high == pulses[used - 1].high) {
        pulses[used - 3].duration +=
            pulses[used - 2].duration + pulses[used - 1].duration;
        used -= 2;
      }
    }
  }
  for (size_t i = 0; i < used; i++)
    record_duration(pulses[i].high, pulses[i].duration);
}
typedef struct {
  bool enable, tune;
  uint32_t frequency, bandwidth;
  uint8_t floor;
  esp_err_t result;
  SemaphoreHandle_t done;
} radio_command_t;
typedef struct {
  radio_command_t *command;
  size_t symbols;
} radio_event_t;
static bool IRAM_ATTR rx_done(rmt_channel_handle_t channel,
                              const rmt_rx_done_event_data_t *data, void *arg) {
  (void)channel;
  (void)arg;
  radio_event_t event = {.symbols = data->num_symbols};
  BaseType_t wake = pdFALSE;
  if (xQueueSendFromISR(events, &event, &wake) != pdTRUE) {
    portENTER_CRITICAL_ISR(&status_lock);
    status.dropped++;
    portEXIT_CRITICAL_ISR(&status_lock);
  }
  return wake == pdTRUE;
}
static esp_err_t arm_rx(void) {
  const rmt_receive_config_t config = {
      .signal_range_min_ns =
          1000,  // ESP32 hardware filter uses the 80 MHz clock.
                 // Longer glitches are merged in software.
      .signal_range_max_ns =
          5000000  // >4 ms protocol packet gaps end a capture.
  };
  return rmt_receive(rx_channel, symbols, sizeof(symbols), &config);
}
static esp_err_t configure_chip(uint32_t frequency, uint32_t bandwidth,
                                uint8_t floor) {
#define CHECK(call)                \
  do {                             \
    esp_err_t err = (call);        \
    if (err != ESP_OK) return err; \
  } while (0)
  CHECK(sx127x_set_opmod(SX127X_MODE_STANDBY, SX127X_MODULATION_OOK, &chip));
  CHECK(sx127x_set_frequency(frequency, &chip));
  CHECK(sx127x_fsk_ook_set_bitrate(1200, &chip));
  CHECK(sx127x_fsk_ook_rx_set_bandwidth(bandwidth, &chip));
  CHECK(sx127x_rx_set_lna_gain(SX127X_LNA_GAIN_AUTO, &chip));
  CHECK(sx127x_ook_rx_set_peak_mode(SX127X_0_5_DB, floor, SX127X_1_1_CHIP,
                                    &chip));
  CHECK(sx127x_fsk_ook_rx_set_trigger(SX127X_RX_TRIGGER_NONE, &chip));
  // PWM pulse widths are asynchronous to the configured bitrate. Disable
  // the bit synchronizer so DIO2 preserves the original pulse timing.
  uint8_t ook_peak;
  CHECK(sx127x_read_register(REGOOKPEAK, &chip.spi_device, &ook_peak));
  CHECK(sx127x_write_register(REGOOKPEAK, ook_peak & ~0x20, &chip.spi_device));
  // Continuous data mode exposes unsliced OOK on DIO2 instead of the packet
  // FIFO.
  uint8_t packet_config;
  CHECK(
      sx127x_read_register(REGPACKETCONFIG2, &chip.spi_device, &packet_config));
  CHECK(sx127x_write_register(REGPACKETCONFIG2, packet_config & ~0x40,
                              &chip.spi_device));
  CHECK(sx127x_set_opmod(SX127X_MODE_RX_CONT, SX127X_MODULATION_OOK, &chip));
  // The upstream packet-mode helper assigns DIO2 SyncAddress; direct RX uses
  // Data.
  CHECK(sx127x_write_register(REGDIOMAPPING1, 0, &chip.spi_device));
  return ESP_OK;
#undef CHECK
}
static void record_duration(bool high, uint32_t duration) {
  weather_packet_t packet;
  if (!duration) return;
  portENTER_CRITICAL(&status_lock);
  status.durations++;
  portEXIT_CRITICAL(&status_lock);
  if (!weather_decode_duration(&decoder, high, duration, &packet)) return;
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&status_lock);
  history[history_next] = (radio_reading_t){
      .packet = packet, .received_us = now, .rssi_dbm = status.rssi_dbm};
  unsigned slot = 0;
  for (; slot < seen_count; slot++) {
    const weather_packet_t *old = &seen_sensors[slot].reading.packet;
    if (old->model == packet.model && old->id == packet.id &&
        old->channel == packet.channel)
      break;
  }
  if (slot == seen_count) {
    if (seen_count < RADIO_SENSORS_MAX)
      seen_count++;
    else {
      slot = 0;
      for (unsigned i = 1; i < seen_count; i++)
        if (seen_sensors[i].reading.received_us <
            seen_sensors[slot].reading.received_us)
          slot = i;
    }
    memset(&seen_sensors[slot], 0, sizeof(seen_sensors[slot]));
  }
  weather_packet_t known = packet;
  const weather_packet_t *old = &seen_sensors[slot].reading.packet;
  if (!known.has_rain && old->has_rain) {
    known.has_rain = true;
    known.rain_raw = old->rain_raw;
    known.rain_mm = old->rain_mm;
  }
  seen_sensors[slot].reading = (radio_reading_t){
      .packet = known, .received_us = now, .rssi_dbm = status.rssi_dbm};
  seen_sensors[slot].packets++;
  history_next = (history_next + 1) % RADIO_HISTORY;
  if (history_count < RADIO_HISTORY) history_count++;
  status.packets++;
  portEXIT_CRITICAL(&status_lock);
  ESP_LOGI(
      TAG, "%s id=%u type=%u rain_raw=%u RSSI=%.1f",
      packet.model == WEATHER_LACROSSE_TX5U ? "LaCrosse-TX5U" : "Acurite-5n1",
      packet.id, packet.message_type, packet.rain_raw, status.rssi_dbm);
}
static void radio_task(void *arg) {
  (void)arg;
  bool enabled = config_get()->radio_enabled != 0, channel_enabled = true;
  esp_err_t err = enabled ? arm_rx()
                          : sx127x_set_opmod(SX127X_MODE_SLEEP,
                                             SX127X_MODULATION_OOK, &chip);
  if (err != ESP_OK) enabled = false;
  portENTER_CRITICAL(&status_lock);
  status.error = err;
  status.receiving = enabled;
  portEXIT_CRITICAL(&status_lock);
  for (;;) {
    radio_event_t event;
    xQueueReceive(events, &event, portMAX_DELAY);
    if (event.command) {
      radio_command_t *cmd = event.command;
      if (channel_enabled) rmt_disable(rx_channel);
      channel_enabled = false;
      // Cancelled receive completions refer to the old capture buffer.
      radio_event_t stale;
      while (xQueueReceive(events, &stale, 0) == pdTRUE) {
        if (stale.command) {
          stale.command->result = ESP_ERR_INVALID_STATE;
          xSemaphoreGive(stale.command->done);
        }
      }
      memset(&decoder, 0, sizeof(decoder));
      enabled = cmd->enable;
      radio_status_t current;
      radio_status(&current);
      cmd->result = enabled
                        ? configure_chip(
                              cmd->tune ? cmd->frequency : current.frequency_hz,
                              cmd->tune ? cmd->bandwidth : current.bandwidth_hz,
                              cmd->tune ? cmd->floor : current.ook_floor)
                        : sx127x_set_opmod(SX127X_MODE_SLEEP,
                                           SX127X_MODULATION_OOK, &chip);
      if (cmd->result == ESP_OK && enabled) {
        cmd->result = rmt_enable(rx_channel);
        if (cmd->result == ESP_OK) {
          channel_enabled = true;
          cmd->result = arm_rx();
        }
      }
      if (cmd->result != ESP_OK) enabled = false;
      portENTER_CRITICAL(&status_lock);
      status.receiving = enabled;
      status.error = cmd->result;
      if (cmd->result == ESP_OK && cmd->tune) {
        status.frequency_hz = cmd->frequency;
        status.bandwidth_hz = cmd->bandwidth;
        status.ook_floor = cmd->floor;
      }
      portEXIT_CRITICAL(&status_lock);
      xSemaphoreGive(cmd->done);
    } else if (enabled) {
      uint8_t raw_rssi;
      if (sx127x_read_register(REGRSSIVALUE_FSK, &chip.spi_device, &raw_rssi) ==
          ESP_OK) {
        portENTER_CRITICAL(&status_lock);
        status.rssi_dbm = -(float)raw_rssi / 2;
        status.bursts++;
        portEXIT_CRITICAL(&status_lock);
      }
      filter_capture(event.symbols);
      // Overflow or capture boundaries may have omitted a gap; don't join
      // unrelated bursts.
      memset(&decoder, 0, sizeof(decoder));
      err = arm_rx();
      if (err != ESP_OK) {
        enabled = false;
        portENTER_CRITICAL(&status_lock);
        status.error = err;
        status.receiving = false;
        portEXIT_CRITICAL(&status_lock);
      }
    }
  }
}
void radio_status(radio_status_t *out) {
  portENTER_CRITICAL(&status_lock);
  *out = status;
  portEXIT_CRITICAL(&status_lock);
}
size_t radio_readings(radio_reading_t *out, size_t max) {
  portENTER_CRITICAL(&status_lock);
  size_t count = history_count < max ? history_count : max;
  for (size_t i = 0; i < count; i++)
    out[i] = history[(history_next + RADIO_HISTORY - 1 - i) % RADIO_HISTORY];
  portEXIT_CRITICAL(&status_lock);
  return count;
}
size_t radio_sensors(radio_sensor_t *out, size_t max) {
  portENTER_CRITICAL(&status_lock);
  size_t count = seen_count < max ? seen_count : max;
  memcpy(out, seen_sensors, count * sizeof(*out));
  portEXIT_CRITICAL(&status_lock);
  return count;
}
static esp_err_t send_command(radio_command_t *cmd) {
  radio_status_t current;
  radio_status(&current);
  if (!current.available) return ESP_ERR_NOT_SUPPORTED;
  StaticSemaphore_t storage;
  cmd->done = xSemaphoreCreateBinaryStatic(&storage);
  radio_event_t event = {.command = cmd};
  xQueueSend(events, &event, portMAX_DELAY);
  xSemaphoreTake(cmd->done, portMAX_DELAY);
  vSemaphoreDelete(cmd->done);
  return cmd->result;
}
esp_err_t radio_receive_enable(bool enable) {
  radio_command_t cmd = {.enable = enable};
  return send_command(&cmd);
}
esp_err_t radio_tune(uint32_t frequency, uint32_t bandwidth, uint8_t floor) {
  // Board antenna/front-end is the 433 MHz variant, not a 915 MHz receiver.
  if (frequency < 400000000 || frequency > 470000000 || bandwidth < 2600 ||
      bandwidth > 250000)
    return ESP_ERR_INVALID_ARG;
  radio_status_t current;
  radio_status(&current);
  radio_command_t cmd = {.enable = current.receiving,
                         .tune = true,
                         .frequency = frequency,
                         .bandwidth = bandwidth,
                         .floor = floor};
  return send_command(&cmd);
}
void radio_start(void) {
  esp_err_t err;
#define INIT(call)                  \
  do {                              \
    err = (call);                   \
    if (err != ESP_OK) goto failed; \
  } while (0)
  const gpio_config_t reset = {.pin_bit_mask = 1ULL << BOARD_RADIO_RST_GPIO,
                               .mode = GPIO_MODE_OUTPUT};
  INIT(gpio_config(&reset));
  gpio_set_level(BOARD_RADIO_RST_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(2));  // SX1278 reset requires >=100 us low.
  gpio_set_level(BOARD_RADIO_RST_GPIO, 1);
  vTaskDelay(
      pdMS_TO_TICKS(10));  // Datasheet allows 5 ms before the first SPI access.
  spi_bus_config_t bus = {.mosi_io_num = BOARD_RADIO_MOSI_GPIO,
                          .miso_io_num = BOARD_RADIO_MISO_GPIO,
                          .sclk_io_num = BOARD_RADIO_SCLK_GPIO,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = 256};
  INIT(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
  const spi_device_interface_config_t spi_config = {
      .clock_speed_hz = 4000000,
      .mode = 0,
      .spics_io_num = BOARD_RADIO_CS_GPIO,
      .queue_size = 1,
      .address_bits = 8};
  INIT(spi_bus_add_device(SPI2_HOST, &spi_config, &spi));
  INIT(sx127x_create(spi, &chip));
  INIT(configure_chip(433920000, 250000, 15));
  rmt_rx_channel_config_t rx = {.gpio_num = BOARD_RADIO_DIO2_GPIO,
                                .clk_src = RMT_CLK_SRC_DEFAULT,
                                .resolution_hz = 1000000,
                                .mem_block_symbols = 256};
  INIT(rmt_new_rx_channel(&rx, &rx_channel));
  const rmt_rx_event_callbacks_t callbacks = {.on_recv_done = rx_done};
  INIT(rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL));
  events = xQueueCreate(8, sizeof(radio_event_t));
  if (!events) {
    err = ESP_ERR_NO_MEM;
    goto failed;
  }
  INIT(rmt_enable(rx_channel));
  status = (radio_status_t){.available = true,
                            .chip_version = chip.chip_version,
                            .frequency_hz = 433920000,
                            .bandwidth_hz = 250000,
                            .ook_floor = 15};
  if (xTaskCreate(radio_task, "radio-rx", 4096, NULL, 3, NULL) != pdPASS) {
    err = ESP_ERR_NO_MEM;
    goto failed;
  }
  ESP_LOGI(TAG, "SX1278 v0x%02x OOK RX 433.920 MHz, DIO2 GPIO%d",
           chip.chip_version, BOARD_RADIO_DIO2_GPIO);
  return;
failed:
  status.available = false;
  status.error = err;
  if (spi) sx127x_set_opmod(SX127X_MODE_SLEEP, SX127X_MODULATION_OOK, &chip);
  if (rx_channel) {
    rmt_disable(rx_channel);
    rmt_del_channel(rx_channel);
    rx_channel = NULL;
  }
  if (events) {
    vQueueDelete(events);
    events = NULL;
  }
  if (spi) {
    spi_bus_remove_device(spi);
    spi_bus_free(SPI2_HOST);
    spi = NULL;
  }
  ESP_LOGE(TAG, "Radio unavailable: %s (0x%x)", esp_err_to_name(err),
           (unsigned)err);
#undef INIT
}
#else
size_t radio_sensors(radio_sensor_t *out, size_t max) {
  (void)out;
  (void)max;
  return 0;
}
void radio_start(void) {}
void radio_status(radio_status_t *out) {
  *out = (radio_status_t){.error = ESP_ERR_NOT_SUPPORTED};
}
size_t radio_readings(radio_reading_t *out, size_t max) {
  (void)out;
  (void)max;
  return 0;
}
esp_err_t radio_receive_enable(bool enable) {
  (void)enable;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t radio_tune(uint32_t f, uint32_t b, uint8_t t) {
  (void)f;
  (void)b;
  (void)t;
  return ESP_ERR_NOT_SUPPORTED;
}
#endif
