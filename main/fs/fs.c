#include "fs.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

bool fs_mount(void) {
  esp_vfs_littlefs_conf_t conf = {
      .base_path = FS_BASE,
      .partition_label = "storage",
      .format_if_mount_failed = true,  // blank partition on first boot
      .dont_mount = false,
  };
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
    return false;
  }
  size_t total = 0, used = 0;
  if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
    ESP_LOGI(TAG, "littlefs mounted at %s (%u/%u bytes used)", FS_BASE,
             (unsigned)used, (unsigned)total);
  } else {
    ESP_LOGI(TAG, "littlefs mounted at %s", FS_BASE);
  }
  return true;
}
