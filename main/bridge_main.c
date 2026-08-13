// Rainlog Wireless Bridge - firmware entry point.
//
// Implemented: WiFi host (SoftAP) + client (STA) passthrough, DNS spoof/proxy,
// HTTP capture of the WU upload, async forward to Rainlog (+ optional real WU),
// LCD UI, SoftAP web configurator (NVS-backed), factory reset via BOOT hold,
// OTA updates (auto-check + web-triggered apply with rollback).
// RGB LED shows status (solid red on error).

#include "activity.h"
#include "ap_clients.h"
#include "button.h"
#include "capture_server.h"
#include "config_server.h"
#include "config_store.h"
#include "dns_server.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs.h"
#include "nvs_flash.h"
#include "ota_update.h"
#include "status_led.h"
#include "time_sync.h"
#include "ui.h"
#include "upload_stats.h"
#include "wifi_link.h"

static const char *TAG = "rainlog-bridge";

static void init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// Dynamic frequency scaling to keep the board cooler: idle the CPU down to
// min_freq, scale up to max_freq under load. Cap max at 80MHz (plenty for our
// load: a TLS forward every few minutes + a 1Hz LCD redraw). Light sleep stays
// off so the SoftAP keeps serving the station.
static void init_power_save(void) {
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 40,
      .light_sleep_enable = false,
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm));
#endif
}

void app_main(void) {
  ESP_LOGI(TAG, "Rainlog Wireless Bridge starting");

  status_led_init();
  status_led_set_provisioning(true);  // until the uplink is up
  init_nvs();
  fs_mount();           // LittleFS for persisted stats (formats on first boot)
  upload_stats_init();  // after fs_mount: loads the persisted total/24h window
  config_load();
  activity_init();
  init_power_save();
  button_init();
  ui_start();

  wifi_link_start();
  ap_clients_init();  // after wifi_link_start: needs the default event loop
  time_sync_start();
  dns_server_start();
  forwarder_init();
  capture_server_start();
  config_server_start();
  ota_update_start();

  // 1s tick so the LED leaves "provisioning" promptly once the uplink is up;
  // the status heartbeat logs once a minute to keep the console readable.
  int tick = 0;
  bool marked_valid = false;
  while (true) {
    if (wifi_link_sta_has_ip()) {
      status_led_set_provisioning(false);
    }
    // Health check for OTA rollback: once we've run ~20s past boot without a
    // crash (servers + UI up), accept this image so the bootloader keeps it.
    // A new image that crash-loops before here gets rolled back automatically.
    if (!marked_valid && tick >= 20) {
      ota_update_mark_valid();
      marked_valid = true;
    }
    if (tick++ % 60 == 0) {
      ESP_LOGI(TAG,
               "uplink=%s  rl(24h)=%d  rl total=%lu  wu(24h)=%d  retry=%d  "
               "free heap=%u  min free=%u",
               wifi_link_sta_has_ip() ? "up" : "down",
               upload_stats_count_last_24h(UPLOAD_TARGET_RL),
               (unsigned long)upload_stats_total(UPLOAD_TARGET_RL),
               upload_stats_count_last_24h(UPLOAD_TARGET_WU),
               forwarder_pending_count(), (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size());
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
