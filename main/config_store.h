// Rainlog Wireless Bridge - runtime configuration store (NVS-backed).
//
// Single source of truth for all provisionable settings. Loaded at boot from
// NVS, falling back to the compile-time defaults in config.h for any unset
// field. The web configurator writes here; wifi_link / forwarder / ui read
// here. Host/path and display defaults also support NVS overrides through
// the optional serial debug console.
//
// The station console is configured with its Rainlog credentials (station id
// "Rainlog<gaugeId>" + the gauge's PWS key) directly, so the bridge passes its
// upload through to Rainlog unchanged and never stores the Rainlog key. The WU
// map says, per Rainlog gauge id, which Weather Underground credentials to also
// relay that gauge's upload under: rainlog gauge id -> (wu id, wu key).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Max Weather Underground relay mappings (one per gauge the bridge serves).
#define WU_MAP_MAX 8

typedef struct {
  uint32_t gauge_id;  // Rainlog gauge id (map key); 0 = unused slot
  char wu_id[64];     // WU station id to relay this gauge's uploads under
  char wu_key[65];    // WU station key/password
} wu_mapping_t;

typedef struct {
  char sta_ssid[33];  // home WiFi (STA uplink)
  char sta_pass[65];
  char ap_ssid[33];  // SoftAP the station joins
  // The bridge WiFi password is also the setup password: it gates joining
  // the AP (WPA2) and, via a sign-in session or HTTP Basic auth, opening the
  // config page from the home LAN.
  char ap_pass[65];
  wu_mapping_t wu_map[WU_MAP_MAX];  // gauge id -> WU relay credentials
  uint8_t wu_map_count;             // active entries in wu_map
  char rainlog_host[128];
  char wu_host[128];
  char wu_update_path[128];
  char ota_host[128];
  char ota_manifest_path[192];
  uint32_t display_full_pct;
  uint32_t display_dim_pct;
  uint32_t display_dim_after_s;  // 0 disables idle dimming
  uint32_t led_level;  // 0 disables status LED; LILYGO LED is on/off only
  bool provisioned;    // true once saved via the configurator
} bridge_config_t;

// Load config from NVS into the in-RAM cache (defaults from config.h for any
// unset field). Call once at boot, after NVS init, before wifi_link_start.
void config_load(void);

// Current cached config (valid after config_load).
const bridge_config_t *config_get(void);

// The WU relay mapping for a Rainlog gauge id, or NULL if none is configured.
const wu_mapping_t *config_find_wu_mapping(uint32_t gauge_id);

// Parse a Rainlog gauge id out of a station-id string. The Rainlog station id
// is the literal "Rainlog" + the gauge number ("Rainlog12345"); a bare number
// is also accepted unless require_prefix is set (set it when sniffing a
// console upload's ID field, where a bare number is NOT a Rainlog station).
// Returns 0 if s is in neither form.
uint32_t config_parse_gauge_id(const char *s, bool require_prefix);

// Persist cfg to NVS, mark provisioned, and update the cache.
esp_err_t config_save(const bridge_config_t *cfg);

// True if a configurator has saved real settings (vs. compile-time defaults).
bool config_is_provisioned(void);

// Erase all saved settings (factory reset). The next boot loads compile-time
// defaults and is unprovisioned. Caller typically reboots right after.
esp_err_t config_clear(void);

// Scalar settings metadata shared by persistence and the debug console.
typedef struct {
  const char *name;
  size_t offset, size;  // strings include NUL; numeric fields are uint32_t
  uint32_t maximum;     // zero identifies a string field
} config_field_t;
extern const config_field_t config_fields[];
extern const size_t config_field_count;
// NULL on success; otherwise a static validation error. Called by config_save.
const char *config_validate(const bridge_config_t *cfg);

// Persist a settings snapshot, preserving its provisioning state.
esp_err_t config_update(const bridge_config_t *cfg);
