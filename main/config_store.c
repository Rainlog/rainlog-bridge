#include "config_store.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "activity.h"
#include "board.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "net/ap_client_gauges.h"
#include "nvs.h"
#include "radio/weather_decode.h"

static const char *TAG = "config";
#define NS "bridge_cfg"

static bridge_config_t s_cfg;

#define STRING_FIELD(name)                 \
  {#name, offsetof(bridge_config_t, name), \
   sizeof(((bridge_config_t *)0)->name), 0}
#define NUMBER_FIELD(name, max) \
  {#name, offsetof(bridge_config_t, name), sizeof(uint32_t), max}
const config_field_t config_fields[] = {
#if RAINLOG_RADIO
    NUMBER_FIELD(radio_enabled, 1),
#endif
    NUMBER_FIELD(bridge_wifi_auto_off, 1),
    STRING_FIELD(sta_ssid),
    STRING_FIELD(sta_pass),
    STRING_FIELD(ap_ssid),
    STRING_FIELD(ap_pass),
    STRING_FIELD(rainlog_host),
    STRING_FIELD(wu_host),
    STRING_FIELD(wu_update_path),
    STRING_FIELD(ota_host),
    STRING_FIELD(ota_manifest_path),
    NUMBER_FIELD(display_full_pct, 100),
    NUMBER_FIELD(display_dim_pct, 100),
    NUMBER_FIELD(display_dim_after_s, UINT32_MAX),
    NUMBER_FIELD(led_level, 255)};
const size_t config_field_count =
    sizeof(config_fields) / sizeof(config_fields[0]);
// NVS keys are limited to 15 bytes, independent of descriptive API names.
static const char *field_keys[] = {
#if RAINLOG_RADIO
    "radio_enabled",
#endif
    "ap_auto_off",   "sta_ssid", "sta_pass",  "ap_ssid",  "ap_pass",
    "rl_host",       "wu_host",  "wu_path",   "ota_host", "ota_path",
    "disp_full",     "disp_dim", "disp_idle", "led_level"};
_Static_assert(sizeof(field_keys) / sizeof(field_keys[0]) ==
                   sizeof(config_fields) / sizeof(config_fields[0]),
               "setting keys");
const char *config_validate(const bridge_config_t *cfg) {
  for (size_t i = 0; i < config_field_count; i++) {
    const config_field_t *f = &config_fields[i];
    const char *value = (const char *)cfg + f->offset;
    if (f->maximum) {
      if (*(const uint32_t *)value > f->maximum)
        return "numeric setting out of range";
    } else if (!memchr(value, 0, f->size))
      return "setting too long";
  }
  if (!cfg->ap_ssid[0]) return "Bridge Wi-Fi SSID required";
  if (strlen(cfg->ap_pass) < 8)
    return "Bridge Wi-Fi password must be at least 8 characters";
  const char *hosts[] = {cfg->rainlog_host, cfg->wu_host, cfg->ota_host};
  for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
    if (!hosts[i][0]) return "host required";
    for (const unsigned char *p = (const unsigned char *)hosts[i]; *p; p++)
      if (*p <= 32 || *p >= 127 || strchr("/:?#@", *p))
        return "host must be a hostname without scheme, path or port";
  }
  if (cfg->wu_update_path[0] != '/' || cfg->ota_manifest_path[0] != '/')
    return "paths must begin with /";
  if (strpbrk(cfg->wu_update_path, "\r\n ?#") ||
      strpbrk(cfg->ota_manifest_path, "\r\n ?#"))
    return "invalid path";
#if RAINLOG_RADIO
  if (cfg->radio_map_count > RADIO_MAP_MAX) return "too many radio mappings";
  for (unsigned i = 0; i < cfg->radio_map_count; i++) {
    const radio_mapping_t *m = &cfg->radio_map[i];
    bool supported = false;
#if WEATHER_PROTOCOL_LACROSSE_TX5U
    supported |=
        m->model == WEATHER_LACROSSE_TX5U && m->sensor_id <= 127 && !m->channel;
#endif
#if WEATHER_PROTOCOL_ACURITE_IRIS
    supported |= m->model == WEATHER_ACURITE_5N1 && m->sensor_id <= 4095 &&
                 (m->channel == 'A' || m->channel == 'B' || m->channel == 'C');
#endif
    if (!supported || !m->gauge_id || !m->rainlog_key[0] ||
        !memchr(m->rainlog_key, 0, sizeof(m->rainlog_key)))
      return "invalid radio mapping";
    if (ap_clients_has_gauge(m->gauge_id))
      return "Rainlog gauge is already assigned to a Wi-Fi device";
    for (unsigned j = 0; j < i; j++) {
      const radio_mapping_t *other = &cfg->radio_map[j];
      if ((other->model == m->model && other->sensor_id == m->sensor_id &&
           other->channel == m->channel) ||
          other->gauge_id == m->gauge_id)
        return "duplicate radio sensor or gauge id";
    }
  }
#endif
  if (cfg->wu_map_count > WU_MAP_MAX) return "too many WU mappings";
  for (unsigned i = 0; i < cfg->wu_map_count; i++) {
    const wu_mapping_t *m = &cfg->wu_map[i];
    if (!memchr(m->device, 0, sizeof(m->device)) ||
        (*m->device ? !config_device_identity_valid(m->device) : !m->gauge_id) ||
        !memchr(m->wu_id, 0, sizeof(m->wu_id)) || !m->wu_id[0] ||
        !memchr(m->wu_key, 0, sizeof(m->wu_key))) return "invalid WU mapping";
    for (unsigned j = 0; j < i; j++) {
      const wu_mapping_t *other = &cfg->wu_map[j];
      if (*m->device ? !strcasecmp(other->device, m->device) :
          !*other->device && other->gauge_id == m->gauge_id)
        return "duplicate WU source";
    }
  }
  return NULL;
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
  snprintf(s_cfg.rainlog_host, sizeof(s_cfg.rainlog_host), "%s",
           CFG_RAINLOG_HOST);
  snprintf(s_cfg.wu_host, sizeof(s_cfg.wu_host), "%s", CFG_WU_HOST);
  snprintf(s_cfg.wu_update_path, sizeof(s_cfg.wu_update_path), "%s",
           CFG_WU_UPDATE_PATH);
  snprintf(s_cfg.ota_host, sizeof(s_cfg.ota_host), "%s", CFG_OTA_HOST);
  snprintf(s_cfg.ota_manifest_path, sizeof(s_cfg.ota_manifest_path), "%s",
           CFG_OTA_MANIFEST_PATH);
  s_cfg.display_full_pct = 100;
  s_cfg.display_dim_pct = 6;
  s_cfg.display_dim_after_s = 30;
  s_cfg.led_level = 24;
  memset(s_cfg.wu_map, 0, sizeof(s_cfg.wu_map));
  s_cfg.wu_map_count = 0;
  s_cfg.provisioned = false;
  s_cfg.bridge_wifi_auto_off = 1;
#if RAINLOG_RADIO
  s_cfg.radio_enabled = 1;
  memset(s_cfg.radio_map, 0, sizeof(s_cfg.radio_map));
  s_cfg.radio_map_count = 0;
#endif

  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGI(TAG, "no saved config; using compile-time defaults");
    return;
  }
  for (size_t i = 0; i < config_field_count; i++) {
    const config_field_t *f = &config_fields[i];
    void *value = (char *)&s_cfg + f->offset;
    if (f->maximum) {
      uint32_t number;
      if (nvs_get_u32(h, field_keys[i], &number) == ESP_OK &&
          number <= f->maximum)
        *(uint32_t *)value = number;
    } else {
      size_t length = f->size;
      nvs_get_str(h, field_keys[i], value, &length);
    }
  }
  // v2 has device identities; decode the old layout explicitly on first boot.
  size_t blob_len = sizeof(s_cfg.wu_map);
  if (nvs_get_blob(h, "wu_map_v2", s_cfg.wu_map, &blob_len) == ESP_OK &&
      blob_len == sizeof(s_cfg.wu_map)) {
    uint8_t count = 0;
    nvs_get_u8(h, "wu_n_v2", &count);
    s_cfg.wu_map_count = count <= WU_MAP_MAX ? count : WU_MAP_MAX;
  } else {
    typedef struct { uint32_t gauge_id; char wu_id[64], wu_key[65]; } legacy_wu_t;
    legacy_wu_t old[WU_MAP_MAX] = {0};
    blob_len = sizeof(old);
    memset(s_cfg.wu_map, 0, sizeof(s_cfg.wu_map));
    if (nvs_get_blob(h, "wu_map", old, &blob_len) == ESP_OK && blob_len == sizeof(old)) {
      uint8_t count = 0;
      nvs_get_u8(h, "wu_n", &count);
      s_cfg.wu_map_count = count <= WU_MAP_MAX ? count : WU_MAP_MAX;
      for (unsigned i = 0; i < s_cfg.wu_map_count; i++) {
        s_cfg.wu_map[i].gauge_id = old[i].gauge_id;
        memcpy(s_cfg.wu_map[i].wu_id, old[i].wu_id, sizeof(old[i].wu_id));
        memcpy(s_cfg.wu_map[i].wu_key, old[i].wu_key, sizeof(old[i].wu_key));
      }
    }
  }
#if RAINLOG_RADIO
  size_t radio_len = sizeof(s_cfg.radio_map);
  if (nvs_get_blob(h, "radio_map", s_cfg.radio_map, &radio_len) == ESP_OK &&
      radio_len == sizeof(s_cfg.radio_map)) {
    uint8_t count = 0;
    nvs_get_u8(h, "radio_n", &count);
    s_cfg.radio_map_count = count <= RADIO_MAP_MAX ? count : 0;
  }
#endif
  uint8_t u;
  if (nvs_get_u8(h, "provd", &u) == ESP_OK) {
    s_cfg.provisioned = (u != 0);
  }
  nvs_close(h);
  ESP_LOGI(TAG, "config loaded (provisioned=%d, %u WU mappings)",
           s_cfg.provisioned, (unsigned)s_cfg.wu_map_count);
}

const bridge_config_t *config_get(void) { return &s_cfg; }

bool config_device_identity_valid(const char *device) {
  unsigned model, id, channel;
  int end = 0;
  if (sscanf(device, "radio:%u:%u:%u%n", &model, &id, &channel, &end) == 3 &&
      !device[end]) {
    if (model > 1 || id > UINT16_MAX || channel > UINT8_MAX) return false;
    char canonical[40];
    snprintf(canonical, sizeof(canonical), "radio:%u:%u:%u", model, id, channel);
    return !strcmp(device, canonical);
  }
  if (strlen(device) != 17) return false;
  for (unsigned i = 0; i < 17; i++) {
    if (i % 3 == 2 ? device[i] != ':' : !isxdigit((unsigned char)device[i])) return false;
  }
  return true;
}
const wu_mapping_t *config_find_wu_device(const char *device, uint32_t legacy_gauge) {
  if (device && *device) {
    for (unsigned i = 0; i < s_cfg.wu_map_count; i++)
      if (!strcasecmp(s_cfg.wu_map[i].device, device)) return &s_cfg.wu_map[i];
  }
  return config_find_wu_mapping(legacy_gauge);
}

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
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || v > UINT32_MAX) return 0;
  return *end == '\0' ? (uint32_t)v : 0;
}

const wu_mapping_t *config_find_wu_mapping(uint32_t gauge_id) {
  if (gauge_id == 0) {
    return NULL;
  }
  for (uint8_t i = 0; i < s_cfg.wu_map_count; i++) {
    if (!s_cfg.wu_map[i].device[0] && s_cfg.wu_map[i].gauge_id == gauge_id) {
      return &s_cfg.wu_map[i];
    }
  }
  return NULL;
}

esp_err_t config_update(const bridge_config_t *cfg) {
  if (config_validate(cfg)) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  // First failing write wins; a partial save must not report success.
  for (size_t i = 0; i < config_field_count && err == ESP_OK; i++) {
    const config_field_t *f = &config_fields[i];
    const void *value = (const char *)cfg + f->offset;
    err = f->maximum ? nvs_set_u32(h, field_keys[i], *(const uint32_t *)value)
                     : nvs_set_str(h, field_keys[i], value);
  }
  if (err == ESP_OK)
    err = nvs_set_blob(h, "wu_map_v2", cfg->wu_map, sizeof(cfg->wu_map));
  if (err == ESP_OK) err = nvs_set_u8(h, "wu_n_v2", cfg->wu_map_count);
#if RAINLOG_RADIO
  if (err == ESP_OK)
    err = nvs_set_blob(h, "radio_map", cfg->radio_map, sizeof(cfg->radio_map));
  if (err == ESP_OK) err = nvs_set_u8(h, "radio_n", cfg->radio_map_count);
#endif
  if (err == ESP_OK) err = nvs_set_u8(h, "provd", cfg->provisioned);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    s_cfg = *cfg;
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

esp_err_t config_save(const bridge_config_t *cfg) {
  bridge_config_t provisioned = *cfg;
  provisioned.provisioned = true;
  return config_update(&provisioned);
}

#if RAINLOG_RADIO
const radio_mapping_t *config_find_radio_mapping(uint8_t model,
                                                 uint32_t sensor_id,
                                                 char channel) {
  for (unsigned i = 0; i < s_cfg.radio_map_count; i++) {
    const radio_mapping_t *m = &s_cfg.radio_map[i];
    if (m->model == model && m->sensor_id == sensor_id && m->channel == channel)
      return m;
  }
  return NULL;
}
#endif

bool config_gauge_uses_radio(uint32_t gauge_id) {
#if RAINLOG_RADIO
  for (unsigned i = 0; i < s_cfg.radio_map_count; i++)
    if (s_cfg.radio_map[i].gauge_id == gauge_id) return true;
#else
  (void)gauge_id;
#endif
  return false;
}
