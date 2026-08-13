#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "activity.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "config";
#define NS "bridge_cfg"

static bridge_config_t s_cfg;

// Overwrite `out` from NVS key only if present; otherwise leave the default.
static void load_str(nvs_handle_t h, const char *key, char *out, size_t outsz) {
  size_t len = outsz;
  nvs_get_str(h, key, out, &len);  // leaves out unchanged on error
}

void config_load(void) {
  // Start from compile-time defaults.
  snprintf(s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid), "%s", CFG_WIFI_STA_SSID);
  snprintf(s_cfg.sta_pass, sizeof(s_cfg.sta_pass), "%s", CFG_WIFI_STA_PASSWORD);
  // Default AP SSID gets a per-device suffix from the MAC (stable across
  // reboots, unique per board) so multiple bridges don't all share one name.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_cfg.ap_ssid, sizeof(s_cfg.ap_ssid), "%s-%02X%02X",
           CFG_WIFI_AP_SSID, mac[4], mac[5]);
  snprintf(s_cfg.ap_pass, sizeof(s_cfg.ap_pass), "%s", CFG_WIFI_AP_PASSWORD);
  memset(s_cfg.wu_map, 0, sizeof(s_cfg.wu_map));
  s_cfg.wu_map_count = 0;
  s_cfg.provisioned = false;

  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGI(TAG, "no saved config; using compile-time defaults");
    return;
  }
  load_str(h, "sta_ssid", s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid));
  load_str(h, "sta_pass", s_cfg.sta_pass, sizeof(s_cfg.sta_pass));
  load_str(h, "ap_ssid", s_cfg.ap_ssid, sizeof(s_cfg.ap_ssid));
  load_str(h, "ap_pass", s_cfg.ap_pass, sizeof(s_cfg.ap_pass));
  // WU map: a single blob of wu_mapping_t entries.
  size_t blob_len = sizeof(s_cfg.wu_map);
  if (nvs_get_blob(h, "wu_map", s_cfg.wu_map, &blob_len) == ESP_OK) {
    uint8_t count = 0;
    nvs_get_u8(h, "wu_n", &count);
    s_cfg.wu_map_count = count <= WU_MAP_MAX ? count : WU_MAP_MAX;
  }
  uint8_t u;
  if (nvs_get_u8(h, "provd", &u) == ESP_OK) {
    s_cfg.provisioned = (u != 0);
  }
  nvs_close(h);
  ESP_LOGI(TAG, "config loaded (provisioned=%d, %u WU mappings)",
           s_cfg.provisioned, (unsigned)s_cfg.wu_map_count);
}

const bridge_config_t *config_get(void) { return &s_cfg; }

uint32_t config_parse_gauge_id(const char *s, bool require_prefix) {
  static const char prefix[] = "Rainlog";
  size_t plen = sizeof(prefix) - 1;
  if (strncasecmp(s, prefix, plen) == 0) {
    s += plen;
  } else if (require_prefix) {
    return 0;
  }
  if (*s < '0' || *s > '9') {
    return 0;  // strtoul would accept leading spaces and signs
  }
  char *end;
  unsigned long v = strtoul(s, &end, 10);
  return *end == '\0' ? (uint32_t)v : 0;
}

const wu_mapping_t *config_find_wu_mapping(uint32_t gauge_id) {
  if (gauge_id == 0) {
    return NULL;
  }
  for (uint8_t i = 0; i < s_cfg.wu_map_count; i++) {
    if (s_cfg.wu_map[i].gauge_id == gauge_id) {
      return &s_cfg.wu_map[i];
    }
  }
  return NULL;
}

esp_err_t config_save(const bridge_config_t *cfg) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  // First failing write wins; a partial save must not report success.
  if (err == ESP_OK) err = nvs_set_str(h, "sta_ssid", cfg->sta_ssid);
  if (err == ESP_OK) err = nvs_set_str(h, "sta_pass", cfg->sta_pass);
  if (err == ESP_OK) err = nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
  if (err == ESP_OK) err = nvs_set_str(h, "ap_pass", cfg->ap_pass);
  if (err == ESP_OK)
    err = nvs_set_blob(h, "wu_map", cfg->wu_map, sizeof(cfg->wu_map));
  if (err == ESP_OK) err = nvs_set_u8(h, "wu_n", cfg->wu_map_count);
  if (err == ESP_OK) err = nvs_set_u8(h, "provd", 1);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    s_cfg = *cfg;
    s_cfg.provisioned = true;
    activity_poke();  // a setting change brightens the screen
    ESP_LOGI(TAG, "config saved");
  }
  return err;
}

bool config_is_provisioned(void) { return s_cfg.provisioned; }

esp_err_t config_clear(void) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_erase_all(h);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  ESP_LOGW(TAG, "config cleared (factory reset)");
  return err;
}
