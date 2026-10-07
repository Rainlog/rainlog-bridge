#include "config_store.h"

#include <assert.h>
#include <string.h>

#include "nvs.h"
struct entry {
  char key[16];
  unsigned char value[2048];
  size_t size;
} entries[24];
static int count, fail_commit;
static struct entry *find(const char *key) {
  for (int i = 0; i < count; i++)
    if (!strcmp(entries[i].key, key)) return &entries[i];
  return NULL;
}
static int put(const char *key, const void *value, size_t size) {
  assert(strlen(key) <= 15 && size <= sizeof(entries[0].value));
  struct entry *e = find(key);
  if (!e) {
    assert(count < 24);
    e = &entries[count++];
    strcpy(e->key, key);
  }
  memcpy(e->value, value, size);
  e->size = size;
  return 0;
}
static int get(const char *key, void *value, size_t *size) {
  struct entry *e = find(key);
  if (!e || *size < e->size) return 1;
  memcpy(value, e->value, e->size);
  *size = e->size;
  return 0;
}
int nvs_open(const char *ns, int mode, int *h) {
  (void)ns;
  (void)mode;
  *h = 1;
  return 0;
}
void nvs_close(int h) { (void)h; }
int nvs_commit(int h) {
  (void)h;
  return fail_commit;
}
int nvs_erase_all(int h) {
  (void)h;
  count = 0;
  return 0;
}
int nvs_set_str(int h, const char *k, const char *v) {
  (void)h;
  return put(k, v, strlen(v) + 1);
}
int nvs_get_str(int h, const char *k, char *v, size_t *s) {
  (void)h;
  return get(k, v, s);
}
int nvs_set_blob(int h, const char *k, const void *v, size_t s) {
  (void)h;
  return put(k, v, s);
}
int nvs_get_blob(int h, const char *k, void *v, size_t *s) {
  (void)h;
  return get(k, v, s);
}
int nvs_set_u32(int h, const char *k, uint32_t v) {
  (void)h;
  return put(k, &v, sizeof(v));
}
int nvs_get_u32(int h, const char *k, uint32_t *v) {
  (void)h;
  size_t s = sizeof(*v);
  return get(k, v, &s);
}
int nvs_set_u8(int h, const char *k, uint8_t v) {
  (void)h;
  return put(k, &v, sizeof(v));
}
int nvs_get_u8(int h, const char *k, uint8_t *v) {
  (void)h;
  size_t s = sizeof(*v);
  return get(k, v, &s);
}
void esp_read_mac(uint8_t *mac, int type) {
  (void)type;
  memset(mac, 0, 6);
  mac[5] = 0x42;
}
void activity_poke(void) {}
int main(void) {
  config_load();
  bridge_config_t cfg = *config_get();
  assert(!cfg.provisioned && cfg.display_full_pct == 100 &&
         cfg.display_dim_after_s == 30);
  assert(!config_validate(&cfg));
  assert(!cfg.wifi_interception_enabled && config_wifi_interception_enabled());
  strcpy(cfg.sta_ssid, "test-network");
  cfg.display_dim_pct = 12;
  cfg.display_dim_after_s = 0;
  cfg.led_level = 0;
  strcpy(cfg.ota_host, "updates.example.org");
  strcpy(cfg.wu_update_path, "/custom/upload");
  cfg.wu_map_count = 1;
  cfg.wu_map[0].gauge_id = 123;
  strcpy(cfg.wu_map[0].wu_id, "TEST");
  strcpy(cfg.wu_map[0].wu_key, "dummy");
  cfg.radio_enabled = 0;
  assert(config_validate(&cfg));
  assert(config_update(&cfg) != 0);
  cfg.wifi_interception_enabled = 1;
  assert(!config_validate(&cfg));
  cfg.wifi_interception_enabled = 0;
  cfg.radio_enabled = 1;
  cfg.radio_map_count = 1;
  cfg.radio_map[0].sensor_id = 4;
  cfg.radio_map[0].gauge_id = 321;
  strcpy(cfg.radio_map[0].rainlog_key, "test-radio-key");
  assert(config_update(&cfg) == 0);
  config_load();
  assert(!memcmp(&cfg, config_get(), sizeof(cfg)));
  assert(config_find_wu_mapping(123) && !config_find_wu_mapping(124));
  assert(config_find_radio_mapping(0, 4, 0)->gauge_id == 321);
  assert(!config_find_radio_mapping(0, 5, 0));
  cfg.radio_map[1] = cfg.radio_map[0];
  cfg.radio_map_count = 2;
  assert(config_validate(&cfg));
  cfg = *config_get();
  cfg.radio_map[0].model = 1;
  cfg.radio_map[0].channel = 'Z';
  assert(config_validate(&cfg));
  cfg = *config_get();
  cfg.radio_map[0].rainlog_key[0] = 0;
  assert(config_validate(&cfg));
  cfg = *config_get();
  cfg.display_dim_pct = 101;
  assert(config_validate(&cfg) && config_update(&cfg) == ESP_ERR_INVALID_ARG);
  cfg = *config_get();
  strcpy(cfg.ota_host, "https://bad.example");
  assert(config_validate(&cfg));
  cfg = *config_get();
  strcpy(cfg.wu_update_path, "not/a/path");
  assert(config_validate(&cfg));
  cfg = *config_get();
  cfg.wu_map[1] = cfg.wu_map[0];
  cfg.wu_map_count = 2;
  assert(config_validate(&cfg));
  cfg = *config_get();
  cfg.display_dim_pct = 13;
  fail_commit = 1;
  assert(config_update(&cfg) != 0 && config_get()->display_dim_pct == 12);
  fail_commit = 0;
  cfg = *config_get();
  assert(config_save(&cfg) == 0 && config_is_provisioned());
  assert(!config_wifi_interception_enabled());
  config_load();
  assert(config_is_provisioned());
  assert(config_parse_gauge_id("Rainlog4294967295", true) == UINT32_MAX);
  assert(config_parse_gauge_id("4294967296", false) == 0);
  assert(config_parse_gauge_id("-1", false) == 0);
  assert(nvs_set_u32(1, "radio_enabled", 0) == 0);
  assert(nvs_set_u32(1, "wifi_capture", 0) == 0);
  config_load();
  assert(config_get()->radio_enabled && !config_get()->wifi_interception_enabled);
  assert(!strcmp(config_get()->sta_ssid, "test-network"));
  assert(config_get()->radio_map[0].gauge_id == 321);
  assert(config_clear() == 0);
  config_load();
  assert(!config_is_provisioned() && config_get()->display_dim_pct == 6);
}
