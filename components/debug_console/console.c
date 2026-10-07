// MeshCore-style newline console, adapted to these boards' RAM and transports.
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "console_line.h"
#include "debug_console.h"
#include "driver/gpio.h"
#include "duktape.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "upload_stats.h"
#include "wifi_link.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

static const char *TAG = "debug";
static console_line_t input_line;

// A bounded allocator keeps debug expressions from exhausting bridge memory.
#define VM_LIMIT (48 * 1024)
static size_t vm_bytes, vm_peak;
static int64_t deadline;
static duk_context *vm;
typedef union {
  size_t size;
  max_align_t alignment;
} allocation_header;
int rainlog_duk_timeout(void *unused) {
  (void)unused;
  // Match MeshCore's 200 ms budget to stop loops without starving bridge tasks.
  return esp_timer_get_time() >= deadline;
}
static void *vm_alloc(void *unused, duk_size_t size) {
  (void)unused;
  if (!size || size > VM_LIMIT - vm_bytes) return NULL;
  allocation_header *h = malloc(sizeof(*h) + size);
  if (!h) return NULL;
  h->size = size;
  vm_bytes += size;
  if (vm_bytes > vm_peak) vm_peak = vm_bytes;
  return h + 1;
}
static void vm_free(void *unused, void *ptr) {
  (void)unused;
  if (!ptr) return;
  allocation_header *h = (allocation_header *)ptr - 1;
  vm_bytes -= h->size;
  free(h);
}
static void *vm_realloc(void *unused, void *ptr, duk_size_t size) {
  if (!ptr) return vm_alloc(unused, size);
  if (!size) {
    vm_free(unused, ptr);
    return NULL;
  }
  allocation_header *h = (allocation_header *)ptr - 1;
  size_t old = h->size;
  if (size > VM_LIMIT - (vm_bytes - old)) return NULL;
  h = realloc(h, sizeof(*h) + size);
  if (!h) return NULL;
  h->size = size;
  vm_bytes = vm_bytes - old + size;
  if (vm_bytes > vm_peak) vm_peak = vm_bytes;
  return h + 1;
}
static void vm_fatal(void *unused, const char *message) {
  (void)unused;
  ESP_LOGE(TAG, "Duktape fatal: %s", message ? message : "unknown");
  abort();
}
static duk_ret_t hw_heap(duk_context *ctx) {
  duk_push_object(ctx);
#define NUMBER_PROPERTY(name, value) \
  duk_push_number(ctx, (value));     \
  duk_put_prop_string(ctx, -2, (name))
  NUMBER_PROPERTY("free", esp_get_free_heap_size());
  NUMBER_PROPERTY("minimum", esp_get_minimum_free_heap_size());
  NUMBER_PROPERTY("largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  NUMBER_PROPERTY("vm", vm_bytes);
  NUMBER_PROPERTY("peak", vm_peak);
  return 1;
}
static duk_ret_t hw_millis(duk_context *ctx) {
  duk_push_number(ctx, esp_timer_get_time() / 1000);
  return 1;
}
static duk_ret_t hw_gpio(duk_context *ctx) {
  double number = duk_require_number(ctx, 0);
  if (!isfinite(number) || number < 0 || number >= GPIO_NUM_MAX ||
      number != (int)number || !GPIO_IS_VALID_GPIO((int)number))
    return duk_error(ctx, DUK_ERR_RANGE_ERROR, "invalid GPIO");
  duk_push_int(ctx, gpio_get_level((int)number));
  return 1;
}
static duk_ret_t hw_wifi(duk_context *ctx) {
  char ap[16], sta[16];
  wifi_link_ap_ip_str(ap, sizeof(ap));
  wifi_link_sta_ip_str(sta, sizeof(sta));
  duk_push_object(ctx);
  duk_push_boolean(ctx, wifi_link_sta_has_ip());
  duk_put_prop_string(ctx, -2, "connected");
  duk_push_string(ctx, ap);
  duk_put_prop_string(ctx, -2, "ap");
  duk_push_string(ctx, sta);
  duk_put_prop_string(ctx, -2, "sta");
  NUMBER_PROPERTY("clients", wifi_link_ap_station_count());
  return 1;
}
static duk_ret_t hw_stats(duk_context *ctx) {
  duk_push_object(ctx);
  NUMBER_PROPERTY("rainlog", upload_stats_total(UPLOAD_TARGET_RL));
  NUMBER_PROPERTY("wu", upload_stats_total(UPLOAD_TARGET_WU));
  NUMBER_PROPERTY("pending", forwarder_pending_count());
  duk_push_string(ctx, forwarder_last_result());
  duk_put_prop_string(ctx, -2, "last");
  return 1;
}
static duk_ret_t js_print(duk_context *ctx) {
  for (duk_idx_t i = 0; i < duk_get_top(ctx); i++)
    printf("%s%s", i ? " " : "", duk_safe_to_string(ctx, i));
  printf("\n");
  return 0;
}
// Local management bindings. Native operations run on the HTTP server task,
// serialized with browser requests; no network authentication gate is changed.
#include "activity.h"
#include "ap_clients.h"
#include "capture_server.h"
#include "config_server.h"
#include "config_store.h"
#include "display.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include "ota_update.h"

typedef struct {
  const char *path, *body, *arg;
  bridge_config_t *settings;
  const bridge_config_t *expected;
  char *json;
  const char *error;
  esp_err_t result;
  SemaphoreHandle_t done;
} management_call;

static void management_work(void *arg) {
  management_call *call = arg;
  const char *path = call->path;
  if (!strcmp(path, "/config")) call->json = config_server_config_json();
#if RAINLOG_RADIO
  else if (!strcmp(path, "/radio"))
    call->json = config_server_radio_json();
#endif
  else if (!strcmp(path, "/scan") || !strcmp(path, "/scanlive"))
    call->json = config_server_scan_json(!strcmp(path, "/scanlive"));
  else if (!strcmp(path, "/clients"))
    call->json = config_server_clients_json(0);
  else if (!strcmp(path, "/rename"))
    call->result = config_server_rename(call->body, call->arg);
  else if (!strcmp(path, "/save")) {
    call->result = config_server_save_form(call->body, &call->error);
    if (call->result == ESP_OK) config_server_restart();
  } else if (!strcmp(path, "/test")) {
    const bridge_config_t *cfg = config_get();
    wifi_link_test_start(
        call->body && call->body[0] ? call->body : cfg->sta_ssid,
        call->arg && call->arg[0] ? call->arg : cfg->sta_pass);
  } else if (!strcmp(path, "/teststatus")) {
    static const char *states[] = {"idle", "running", "ok", "fail"};
    char status[32];
    snprintf(status, sizeof(status), "{\"s\":\"%s\"}",
             states[wifi_link_test_status()]);
    call->json = strdup(status);
  } else if (!strcmp(path, "/ota/status")) {
    char status[256];
    ota_update_status_json(status, sizeof(status));
    call->json = strdup(status);
  } else if (!strcmp(path, "/ota/check"))
    ota_update_request_check();
  else if (!strcmp(path, "/ota/apply"))
    ota_update_request_apply();
  else if (!strcmp(path, "settings.get"))
    *call->settings = *config_get();
  else if (!strcmp(path, "settings.set")) {
    call->error = call->expected && memcmp(call->expected, config_get(),
                                           sizeof(*call->expected))
                      ? "settings changed concurrently; retry"
                      : config_validate(call->settings);
    call->result =
        call->error ? ESP_ERR_INVALID_ARG : config_update(call->settings);
  } else if (!strcmp(path, "reboot"))
    config_server_restart();
  else if (!strcmp(path, "factoryReset")) {
    call->result = config_clear();
    if (call->result == ESP_OK) {
      upload_stats_clear();
      ap_clients_clear_names();
      config_server_restart();
    }
  } else if (!strcmp(path, "stats.clear"))
    upload_stats_clear();
  else
    call->result = ESP_ERR_INVALID_ARG;
  if ((!strcmp(path, "/config") || !strcmp(path, "/scan") ||
       !strcmp(path, "/scanlive") || !strcmp(path, "/clients") ||
#if RAINLOG_RADIO
       !strcmp(path, "/radio") ||
#endif
       !strcmp(path, "/teststatus") || !strcmp(path, "/ota/status")) &&
      !call->json)
    call->result = ESP_ERR_NO_MEM;
  xSemaphoreGive(call->done);
}
static void management_run(duk_context *ctx, management_call *call) {
  StaticSemaphore_t storage;
  call->done = xSemaphoreCreateBinaryStatic(&storage);
  int64_t start = esp_timer_get_time();
  call->result =
      httpd_queue_work(capture_server_httpd(), management_work, call);
  if (call->result == ESP_OK) xSemaphoreTake(call->done, portMAX_DELAY);
  // The JS budget measures script execution, not native Wi-Fi scan/flash time.
  deadline += esp_timer_get_time() - start;
  vSemaphoreDelete(call->done);
  if (call->result != ESP_OK) {
    free(call->json);
    (void)duk_error(ctx, DUK_ERR_ERROR, "%s: %s", call->path,
                    call->error ? call->error : esp_err_to_name(call->result));
  }
}
static duk_ret_t decode_json(duk_context *ctx, void *data) {
  duk_push_string(ctx, data);
  duk_json_decode(ctx, -1);
  return 1;
}
static duk_ret_t js_web(duk_context *ctx) {
  management_call call = {.path = duk_require_string(ctx, 0)};
  static const char *allowed[] = {
#if RAINLOG_RADIO
      "/radio",
#endif
      "/config",    "/scan",  "/scanlive",    "/clients",    "/rename",
      "/save",      "/test",  "/teststatus",  "/ota/status", "/ota/check",
      "/ota/apply", "reboot", "factoryReset", "stats.clear"};
  bool valid = false;
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
    if (!strcmp(call.path, allowed[i])) {
      valid = true;
      break;
    }
  if (!valid) return duk_error(ctx, DUK_ERR_TYPE_ERROR, "unknown web action");
  // Only named management actions are dispatched, never arbitrary upload URLs.
  if (!duk_is_null_or_undefined(ctx, 1)) call.body = duk_require_string(ctx, 1);
  if (!duk_is_null_or_undefined(ctx, 2)) call.arg = duk_require_string(ctx, 2);
  if ((!strcmp(call.path, "/save") || !strcmp(call.path, "/rename")) &&
      !call.body)
    return duk_error(ctx, DUK_ERR_TYPE_ERROR, "argument required");
  if (!strcmp(call.path, "/rename") && !call.arg) call.arg = "";
  management_run(ctx, &call);
  if (!call.json) {
    duk_push_true(ctx);
    return 1;
  }
  // A protected conversion ensures native JSON buffers are freed on VM OOM.
  int result = duk_safe_call(ctx, decode_json, call.json, 0, 1);
  free(call.json);
  if (result != DUK_EXEC_SUCCESS) return duk_throw(ctx);
  return 1;
}
static duk_ret_t js_settings_get(duk_context *ctx) {
  bridge_config_t cfg;
  management_call call = {.path = "settings.get", .settings = &cfg};
  management_run(ctx, &call);
  duk_push_object(ctx);
  for (size_t i = 0; i < config_field_count; i++) {
    const config_field_t *f = &config_fields[i];
    const void *value = (const char *)&cfg + f->offset;
    if (f->maximum == 1)
      duk_push_boolean(ctx, *(const uint32_t *)value != 0);
    else if (f->maximum)
      duk_push_uint(ctx, *(const uint32_t *)value);
    else
      duk_push_string(ctx, value);
    duk_put_prop_string(ctx, -2, f->name);
  }
  duk_push_boolean(ctx, cfg.provisioned);
  duk_put_prop_string(ctx, -2, "provisioned");
  duk_push_array(ctx);
  for (unsigned i = 0; i < cfg.wu_map_count; i++) {
    duk_push_object(ctx);
    NUMBER_PROPERTY("gauge_id", cfg.wu_map[i].gauge_id);
    duk_push_string(ctx, cfg.wu_map[i].wu_id);
    duk_put_prop_string(ctx, -2, "wu_id");
    duk_push_string(ctx, cfg.wu_map[i].wu_key);
    duk_put_prop_string(ctx, -2, "wu_key");
    duk_put_prop_index(ctx, -2, i);
  }
  duk_put_prop_string(ctx, -2, "wu_map");
#if RAINLOG_RADIO
  duk_push_array(ctx);
  for (unsigned i = 0; i < cfg.radio_map_count; i++) {
    duk_push_object(ctx);
    NUMBER_PROPERTY("model", cfg.radio_map[i].model);
    NUMBER_PROPERTY("sensor_id", cfg.radio_map[i].sensor_id);
    NUMBER_PROPERTY("channel", (unsigned char)cfg.radio_map[i].channel);
    NUMBER_PROPERTY("gauge_id", cfg.radio_map[i].gauge_id);
    duk_push_string(ctx, cfg.radio_map[i].rainlog_key);
    duk_put_prop_string(ctx, -2, "rainlog_key");
    duk_put_prop_index(ctx, -2, i);
  }
  duk_put_prop_string(ctx, -2, "radio_map");
#endif
  return 1;
}
static void read_string(duk_context *ctx, duk_idx_t index, const char *key,
                        char *out, size_t capacity) {
  duk_get_prop_string(ctx, index, key);
  duk_size_t length;
  const char *value = duk_require_lstring(ctx, -1, &length);
  if (length >= capacity || memchr(value, 0, length))
    (void)duk_error(ctx, DUK_ERR_RANGE_ERROR, "%s too long or contains NUL",
                    key);
  memcpy(out, value, length);
  out[length] = 0;
  duk_pop(ctx);
}
static duk_ret_t js_settings_set(duk_context *ctx) {
  duk_require_object_coercible(ctx, 0);
  if (duk_is_array(ctx, 0) || !duk_is_object(ctx, 0))
    return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                     "settings patch must be an object");
  bridge_config_t cfg;
  management_call get = {.path = "settings.get", .settings = &cfg};
  management_run(ctx, &get);
  bridge_config_t original = cfg;
  duk_enum(ctx, 0, DUK_ENUM_OWN_PROPERTIES_ONLY);
  while (duk_next(ctx, -1, 1)) {
    const char *name = duk_require_string(ctx, -2);
    if (!strcmp(name, "sta_ssid") || !strcmp(name, "sta_pass") ||
        !strcmp(name, "ap_ssid") || !strcmp(name, "ap_pass") ||
        !strcmp(name, "wu_map")
#if RAINLOG_RADIO
        || !strcmp(name, "wifi_interception_enabled")
#endif
    )
      cfg.provisioned = true;
#if RAINLOG_RADIO
    if (!strcmp(name, "radio_map")) {
      if (!duk_is_array(ctx, -1) || duk_get_length(ctx, -1) > RADIO_MAP_MAX)
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radio_map must be an array of at most %d entries",
                         RADIO_MAP_MAX);
      cfg.radio_map_count = duk_get_length(ctx, -1);
      memset(cfg.radio_map, 0, sizeof(cfg.radio_map));
      for (unsigned i = 0; i < cfg.radio_map_count; i++) {
        duk_get_prop_index(ctx, -1, i);
        duk_idx_t row = duk_get_top_index(ctx);
        radio_mapping_t *m = &cfg.radio_map[i];
        const char *fields[] = {"model", "sensor_id", "channel", "gauge_id"};
        uint32_t values[4];
        for (unsigned j = 0; j < 4; j++) {
          duk_get_prop_string(ctx, row, fields[j]);
          double value = duk_require_number(ctx, -1);
          if (!isfinite(value) || value < 0 || value > UINT32_MAX ||
              value != (uint32_t)value)
            return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                             "invalid radio mapping number");
          values[j] = value;
          duk_pop(ctx);
        }
        if (values[0] > 1 || values[2] > 127)
          return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                           "invalid model or channel");
        m->model = values[0];
        m->sensor_id = values[1];
        m->channel = values[2];
        m->gauge_id = values[3];
        read_string(ctx, row, "rainlog_key", m->rainlog_key,
                    sizeof(m->rainlog_key));
        duk_pop(ctx);
      }
      cfg.provisioned = true;
    } else
#endif
        if (!strcmp(name, "wu_map")) {
      if (!duk_is_array(ctx, -1) || duk_get_length(ctx, -1) > WU_MAP_MAX)
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "wu_map must be an array of at most %d entries",
                         WU_MAP_MAX);
      cfg.wu_map_count = duk_get_length(ctx, -1);
      memset(cfg.wu_map, 0, sizeof(cfg.wu_map));
      for (unsigned i = 0; i < cfg.wu_map_count; i++) {
        duk_get_prop_index(ctx, -1, i);
        duk_idx_t row = duk_get_top_index(ctx);
        duk_get_prop_string(ctx, row, "gauge_id");
        double gauge = duk_require_number(ctx, -1);
        if (!isfinite(gauge) || gauge < 1 || gauge > UINT32_MAX ||
            gauge != (uint32_t)gauge)
          return duk_error(ctx, DUK_ERR_RANGE_ERROR, "invalid gauge_id");
        cfg.wu_map[i].gauge_id = gauge;
        duk_pop(ctx);
        read_string(ctx, row, "wu_id", cfg.wu_map[i].wu_id,
                    sizeof(cfg.wu_map[i].wu_id));
        read_string(ctx, row, "wu_key", cfg.wu_map[i].wu_key,
                    sizeof(cfg.wu_map[i].wu_key));
        duk_pop(ctx);
      }
    } else {
      const config_field_t *f = NULL;
      for (size_t i = 0; i < config_field_count; i++)
        if (!strcmp(config_fields[i].name, name)) {
          f = &config_fields[i];
          break;
        }
      if (!f)
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "unknown or read-only setting: %s", name);
      void *value = (char *)&cfg + f->offset;
      if (f->maximum) {
        double number = f->maximum == 1 && duk_is_boolean(ctx, -1)
                            ? duk_get_boolean(ctx, -1)
                            : duk_require_number(ctx, -1);
        if (!isfinite(number) || number < 0 || number > f->maximum ||
            number != (uint32_t)number)
          return duk_error(ctx, DUK_ERR_RANGE_ERROR, "%s out of range", name);
        *(uint32_t *)value = number;
      } else {
        duk_size_t length;
        const char *text = duk_require_lstring(ctx, -1, &length);
        if (length >= f->size || memchr(text, 0, length))
          return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                           "%s too long or contains NUL", name);
        memcpy(value, text, length);
        ((char *)value)[length] = 0;
      }
    }
    duk_pop_2(ctx);
  }
  duk_pop(ctx);
  management_call set = {
      .path = "settings.set", .settings = &cfg, .expected = &original};
  management_run(ctx, &set);
  duk_push_true(ctx);
  return 1;
}
static duk_ret_t js_build_info(duk_context *ctx) {
  const esp_app_desc_t *app = esp_app_get_description();
  duk_push_object(ctx);
  duk_push_string(ctx, BOARD_ID);
  duk_put_prop_string(ctx, -2, "board");
  duk_push_string(ctx, app->version);
  duk_put_prop_string(ctx, -2, "version");
  duk_push_string(ctx, app->idf_ver);
  duk_put_prop_string(ctx, -2, "idf");
  NUMBER_PROPERTY("display_width", BOARD_DISPLAY_W);
  NUMBER_PROPERTY("display_height", BOARD_DISPLAY_H);
  NUMBER_PROPERTY("font", BOARD_DISPLAY_FONT);
  NUMBER_PROPERTY("button_gpio", BOARD_BOOT_BUTTON_GPIO);
#if BOARD_DISPLAY_SSD1306
  NUMBER_PROPERTY("led_gpio", BOARD_STATUS_LED_GPIO);
  NUMBER_PROPERTY("i2c_sda", BOARD_I2C_SDA_GPIO);
  NUMBER_PROPERTY("i2c_scl", BOARD_I2C_SCL_GPIO);
  NUMBER_PROPERTY("oled_address", BOARD_OLED_ADDRESS);
#else
  NUMBER_PROPERTY("led_gpio", BOARD_RGB_LED_GPIO);
  NUMBER_PROPERTY("lcd_backlight_gpio", BOARD_LCD_BL_GPIO);
#endif
  return 1;
}
static duk_ret_t js_wake(duk_context *ctx) {
  (void)ctx;
  activity_poke();
  return 0;
}
static duk_ret_t js_log_level(duk_context *ctx) {
  const char *tag = duk_require_string(ctx, 0);
  int level = duk_require_int(ctx, 1);
  if (level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE)
    return duk_error(ctx, DUK_ERR_RANGE_ERROR, "log level must be 0..5");
  esp_log_level_set(tag, level);
  return 0;
}
#include "radio.h"
static duk_ret_t js_radio_status(duk_context *ctx) {
  radio_status_t status;
  radio_status(&status);
  duk_push_object(ctx);
  duk_push_boolean(ctx, status.available);
  duk_put_prop_string(ctx, -2, "available");
  duk_push_boolean(ctx, status.receiving);
  duk_put_prop_string(ctx, -2, "receiving");
  NUMBER_PROPERTY("chip_version", status.chip_version);
  NUMBER_PROPERTY("frequency_hz", status.frequency_hz);
  NUMBER_PROPERTY("bandwidth_hz", status.bandwidth_hz);
  NUMBER_PROPERTY("ook_floor", status.ook_floor);
  NUMBER_PROPERTY("bursts", status.bursts);
  NUMBER_PROPERTY("durations", status.durations);
  NUMBER_PROPERTY("packets", status.packets);
  NUMBER_PROPERTY("dropped", status.dropped);
  NUMBER_PROPERTY("rssi_dbm", status.rssi_dbm);
  NUMBER_PROPERTY("error", status.error);
  return 1;
}
static duk_ret_t js_radio_receive(duk_context *ctx) {
  if (!duk_is_boolean(ctx, 0))
    return duk_error(ctx, DUK_ERR_TYPE_ERROR, "receive expects a boolean");
  esp_err_t err = radio_receive_enable(duk_get_boolean(ctx, 0));
  if (err != ESP_OK)
    return duk_error(ctx, DUK_ERR_ERROR, "radio receive: %s",
                     esp_err_to_name(err));
  duk_push_true(ctx);
  return 1;
}
static duk_ret_t js_radio_tune(duk_context *ctx) {
  double frequency = duk_require_number(ctx, 0),
         bandwidth = duk_require_number(ctx, 1),
         floor = duk_require_number(ctx, 2);
  if (!isfinite(frequency) || frequency < 400000000 || frequency > 470000000 ||
      frequency != (uint32_t)frequency || !isfinite(bandwidth) ||
      bandwidth < 2600 || bandwidth > 250000 ||
      bandwidth != (uint32_t)bandwidth || !isfinite(floor) || floor < 0 ||
      floor > 255 || floor != (uint8_t)floor)
    return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                     "radio tune expects 400-470 MHz, 2600-250000 Hz "
                     "bandwidth, floor 0-255 (integers)");
  esp_err_t err = radio_tune(frequency, bandwidth, floor);
  if (err != ESP_OK)
    return duk_error(ctx, DUK_ERR_ERROR, "radio tune: %s",
                     esp_err_to_name(err));
  duk_push_true(ctx);
  return 1;
}
static duk_ret_t js_radio_packets(duk_context *ctx) {
  radio_reading_t readings[RADIO_HISTORY];
  size_t count = radio_readings(readings, RADIO_HISTORY);
  duk_push_array(ctx);
  for (size_t i = 0; i < count; i++) {
    const weather_packet_t *p = &readings[i].packet;
    duk_push_object(ctx);
    duk_push_string(ctx, p->model == WEATHER_LACROSSE_TX5U ? "LaCrosse-TX5U"
                                                           : "Acurite-5n1");
    duk_put_prop_string(ctx, -2, "model");
    NUMBER_PROPERTY("id", p->id);
    NUMBER_PROPERTY("message_type", p->message_type);
    NUMBER_PROPERTY("received_ms", readings[i].received_us / 1000);
    NUMBER_PROPERTY("rssi_dbm", readings[i].rssi_dbm);
    if (p->has_rain) {
      NUMBER_PROPERTY("rain_raw", p->rain_raw);
      NUMBER_PROPERTY("rain_mm", p->rain_mm);
    }
    if (p->model == WEATHER_ACURITE_5N1) {
      char channel[2] = {p->channel, 0};
      duk_push_string(ctx, channel);
      duk_put_prop_string(ctx, -2, "channel");
      duk_push_boolean(ctx, p->battery_ok);
      duk_put_prop_string(ctx, -2, "battery_ok");
      NUMBER_PROPERTY("sequence", p->sequence);
      NUMBER_PROPERTY("wind_kph", p->wind_kph);
      if (p->has_rain) {
        NUMBER_PROPERTY("wind_direction", p->wind_direction);
      }
      if (p->has_temperature) {
        NUMBER_PROPERTY("temperature_c", p->temperature_c);
        NUMBER_PROPERTY("humidity", p->humidity);
      }
    }
    duk_put_prop_index(ctx, -2, i);
  }
  return 1;
}
static void release_vm(void) {
  if (vm) duk_destroy_heap(vm);
  vm = NULL;
}
static bool execute(const char *source) {
  if (!strcmp(source, "off")) {
    release_vm();
    return true;
  }
  if (!vm) {
    vm_peak = 0;
    deadline = esp_timer_get_time() + 200000;
    vm = duk_create_heap(vm_alloc, vm_realloc, vm_free, NULL, vm_fatal);
    if (!vm) {
      printf("VM initialization failed\n");
      return false;
    }
    duk_push_object(vm);
    const struct {
      const char *name;
      duk_c_function function;
      duk_idx_t args;
    } bindings[] = {{"heap", hw_heap, 0},   {"millis", hw_millis, 0},
                    {"gpio", hw_gpio, 1},   {"wifi", hw_wifi, 0},
                    {"stats", hw_stats, 0}, {"buildInfo", js_build_info, 0},
                    {"wake", js_wake, 0},   {"logLevel", js_log_level, 2}};
    for (unsigned i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
      duk_push_c_function(vm, bindings[i].function, bindings[i].args);
      duk_put_prop_string(vm, -2, bindings[i].name);
    }
    duk_put_global_string(vm, "hw");
    duk_push_c_function(vm, js_print, DUK_VARARGS);
    duk_put_global_string(vm, "print");
    duk_push_object(vm);
    duk_push_c_function(vm, js_settings_get, 0);
    duk_put_prop_string(vm, -2, "get");
    duk_push_c_function(vm, js_settings_set, 1);
    duk_put_prop_string(vm, -2, "set");
    duk_put_global_string(vm, "settings");
    duk_push_object(vm);
    duk_push_c_function(vm, js_radio_status, 0);
    duk_put_prop_string(vm, -2, "status");
    duk_push_c_function(vm, js_radio_receive, 1);
    duk_put_prop_string(vm, -2, "receive");
    duk_push_c_function(vm, js_radio_tune, 3);
    duk_put_prop_string(vm, -2, "tune");
    duk_push_c_function(vm, js_radio_packets, 0);
    duk_put_prop_string(vm, -2, "packets");
    duk_put_global_string(vm, "radio");
    duk_push_object(vm);
    duk_push_c_function(vm, js_web, 3);
    duk_put_prop_string(vm, -2, "call");
    duk_put_global_string(vm, "web");
    if (duk_peval_string(
            vm,
            "(function(){var "
            "routes={"
#if RAINLOG_RADIO
            "radio:'/radio',"
#endif
            "config:'/config',scan:'/scan',scanLive:'/"
            "scanlive',clients:'/clients',"
            "save:'/save',rename:'/rename',test:'/test',testStatus:'/"
            "teststatus',otaStatus:'/ota/status',"
            "otaCheck:'/ota/check',otaApply:'/ota/"
            "apply',reboot:'reboot',factoryReset:'factoryReset',clearStats:'"
            "stats.clear'};"
            "Object.keys(routes).forEach(function(name){web[name]=function(a,b)"
            "{return web.call(routes[name],a,b);};});}())") != 0) {
      printf("Console bindings: %s\n", duk_safe_to_string(vm, -1));
      release_vm();
      return false;
    }
    duk_pop(vm);
    printf("Duktape ROM built-ins: VM startup=%u bytes\n", (unsigned)vm_bytes);
  }
  deadline = esp_timer_get_time() + 200000;
  bool success = duk_peval_string(vm, source) == 0;
  printf("%s\n", duk_safe_to_string(vm, -1));
  duk_pop(vm);
  if (!success) release_vm();
  return success;
}

static int read_byte(uint8_t *byte) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return usb_serial_jtag_read_bytes(byte, 1, portMAX_DELAY);
#else
  return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, byte, 1, portMAX_DELAY);
#endif
}

static void console_task(void *unused) {
  (void)unused;
  for (;;) {
    // Wait for one byte, not a full buffer: short commands execute at newline.
    uint8_t byte;
    if (read_byte(&byte) != 1) continue;
    console_line_result_t result = console_line_feed(&input_line, byte);
    if (result == CONSOLE_LINE_INVALID) {
      printf("Result=ERROR invalid or oversized console line; discarded\n");
    } else if (result == CONSOLE_LINE_READY &&
               !strncmp(input_line.bytes, "js ", 3)) {
      bool success = execute(input_line.bytes + 3);
      printf("Result=%s\n", success ? "OK" : "ERROR");
    }
  }
}

void debug_console_start(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 4096,
                                            .tx_buffer_size = 4096};
  esp_err_t err = usb_serial_jtag_driver_install(&config);
  if (err == ESP_OK) usb_serial_jtag_vfs_use_driver();
#else
  esp_err_t err =
      uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 4096, 0, 0, NULL, 0);
  if (err == ESP_OK) uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Console transport: %s", esp_err_to_name(err));
    return;
  }
  if (xTaskCreate(console_task, "debug-console", 12288, NULL, 1, NULL) !=
      pdPASS) {
    ESP_LOGE(TAG, "Unable to create console task");
    return;
  }
  ESP_LOGI(TAG, "Debug console ready: js <source> (Duktape ROM built-ins)");
}
