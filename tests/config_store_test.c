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
static uint32_t wifi_gauge;
bool ap_clients_has_gauge(uint32_t gauge) { return gauge && gauge == wifi_gauge; }
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
  assert(cfg.bridge_wifi_auto_off);
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
  assert(!config_validate(&cfg));
  cfg.bridge_wifi_auto_off = 0;
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
  assert(config_gauge_uses_radio(321) && !config_gauge_uses_radio(322));
  wifi_gauge = 321;
  assert(strstr(config_validate(&cfg), "Wi-Fi device"));
  assert(config_save(&cfg) == ESP_ERR_INVALID_ARG);
  wifi_gauge = 0;
  assert(!config_validate(&cfg));
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
  cfg.wu_map[1] = (wu_mapping_t){.device="radio:0:7:0", .wu_id="RADIO", .wu_key="test-key"};
  cfg.wu_map_count = 2;
  assert(!config_validate(&cfg) && !config_update(&cfg));
  config_load();
  assert(config_find_wu_device("radio:0:7:0", 123) == &config_get()->wu_map[1]);
  assert(config_find_wu_device("01:02:03:04:05:06", 123) == &config_get()->wu_map[0]);
  assert(!config_find_wu_device("radio:1:7:67", 0));
  cfg.wu_map[0] = cfg.wu_map[1];
  assert(config_validate(&cfg));
  assert(!config_device_identity_valid("radio:0:70000:0"));
  assert(!config_device_identity_valid("radio:0:7:0junk"));
  assert(!config_device_identity_valid("01:02:03:04:05:GG"));
  cfg = *config_get();
  cfg.display_dim_pct = 13;
  fail_commit = 1;
  assert(config_update(&cfg) != 0 && config_get()->display_dim_pct == 12);
  fail_commit = 0;
  cfg = *config_get();
  assert(config_save(&cfg) == 0 && config_is_provisioned());
  assert(!config_get()->bridge_wifi_auto_off);
  config_load();
  assert(config_is_provisioned());
  assert(config_parse_gauge_id("Rainlog4294967295", true) == UINT32_MAX);
  assert(config_parse_gauge_id("4294967296", false) == 0);
  assert(config_parse_gauge_id("-1", false) == 0);
  assert(nvs_set_u32(1, "radio_enabled", 0) == 0);
  assert(nvs_set_u32(1, "ap_auto_off", 0) == 0);
  config_load();
  assert(!config_get()->radio_enabled && !config_get()->bridge_wifi_auto_off);
  assert(!strcmp(config_get()->sta_ssid, "test-network"));
  assert(config_get()->radio_map[0].gauge_id == 321);
  assert(config_clear() == 0);
  config_load();
  assert(!config_is_provisioned() && config_get()->display_dim_pct == 6);
  cfg = *config_get();
  assert(!config_bridge_wifi_required(&cfg));
  cfg.wu_map_count = 1;
  strcpy(cfg.wu_map[0].device, "radio:0:7:0");
  assert(!config_bridge_wifi_required(&cfg));
  strcpy(cfg.wu_map[0].device, "00:0F:55:A1:17:BB");
  assert(config_bridge_wifi_required(&cfg));
  cfg.wu_map[0].device[0] = 0;
  cfg.wu_map[0].gauge_id = 321;
  assert(config_bridge_wifi_required(&cfg));
  cfg.radio_map_count = 1;
  cfg.radio_map[0].gauge_id = 321;
  assert(!config_bridge_wifi_required(&cfg));
  cfg.wu_map_count = 0;
  cfg.radio_map_count = 0;
  config_note_wifi_uploader();
  config_load();
  assert(config_bridge_wifi_required(config_get()));
  assert(config_update(&cfg) == 0);
  config_load();
  assert(config_bridge_wifi_required(config_get()));
  assert(config_clear() == 0);
  config_load();
  assert(!config_bridge_wifi_required(config_get()));
  // A real pre-device-identity blob must survive upgrade and a v2 round trip.
  count = 0;
  typedef struct { uint32_t gauge_id; char wu_id[64], wu_key[65]; } old_mapping_t;
  old_mapping_t legacy[WU_MAP_MAX] = {{.gauge_id=987, .wu_id="OLD", .wu_key="old-key"}};
  put("wu_map", legacy, sizeof(legacy));
  nvs_set_u8(1, "wu_n", 1);
  config_load();
  assert(config_get()->wu_map_count == 1);
  const wu_mapping_t *old = config_find_wu_mapping(987);
  assert(old && !strcmp(old->wu_id, "OLD") && !strcmp(old->wu_key, "old-key") && !*old->device);
  cfg = *config_get();
  assert(!config_update(&cfg));
  config_load();
  old = config_find_wu_mapping(987);
  assert(old && !strcmp(old->wu_key, "old-key"));
}
