// Rainlog Wireless Bridge - SoftAP client registry.
//
// Remembers every device that joins the bridge's SoftAP (RAM only; resets on
// reboot): MAC, connected state, last-seen time, last DHCP address, and the
// last Rainlog station ID captured from a weather upload.
// The config page's Devices tab reads this via GET /clients to show what is
// on the bridge's WiFi and link to each device's own page.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

// More than the SoftAP's 4-connection limit, so devices that come and go
// (phone, replaced console) stay listed after they disconnect.
#define AP_CLIENTS_MAX 8

// Sticky per-device problem flags (RAM, reset on reboot). Flags rather than
// counters: "has this ever happened" is all the table can afford to store.
#define AP_CLIENT_ERR_RL_FAIL 0x01    // Rainlog unreachable (no response)
#define AP_CLIENT_ERR_RL_REJECT 0x02  // Rainlog answered but did not accept
#define AP_CLIENT_ERR_WU_FAIL 0x04    // WU unreachable (no response)
#define AP_CLIENT_ERR_WU_REJECT 0x08  // WU answered but did not accept

typedef struct {
  uint8_t mac[6];
  bool connected;
  int64_t last_seen_us;  // esp_timer time of the last join/leave/poll sighting
  // esp_timer time of the device's last captured weather upload; 0 = none yet
  // this boot. The status screen lists devices that have forwarded.
  int64_t last_upload_us;
  uint32_t gauge_id;   // last Rainlog station ID captured from this device
  uint32_t rx_count;   // weather uploads captured from this device (this boot)
  uint32_t fwd_count;  // of those, accepted by Rainlog (this boot)
  uint32_t wu_count;   // of those, relayed to Weather Underground (this boot)
  uint8_t err_flags;   // AP_CLIENT_ERR_* bits seen this boot
  // The last reject reason from each upstream (the response body it returned
  // with a non-accept, e.g. "RATELIMIT" / "INVALIDPASSWORDID"); empty if none.
  // Detail behind the generic *_REJECT flags.
  char rl_reject[24];
  char wu_reject[24];
  esp_ip4_addr_t ip;   // last known DHCP address; 0 if never leased
  int rssi;            // live signal (dBm); only meaningful while connected
  const char *vendor;  // static OUI label ("Private MAC"/"Unknown" fallback)
  // Hostname the device announced in its DHCP lease (option 12, sanitized by
  // esp-netif); empty if it sent none.
  char hostname[64];
  // User-given name from the config page (NVS-persisted per MAC); empty if
  // never named.
  char name[33];
} ap_client_t;

// Register the WiFi event handlers and init the table. Call once after
// wifi_link_start (needs the default event loop).
void ap_clients_init(void);

// Copy up to `max` tracked clients into `out`, refreshed against the live
// associated-station list (RSSI + DHCP IPs; also self-heals missed join/leave
// events). Returns the count.
int ap_clients_snapshot(ap_client_t *out, int max);

// Record that the device currently holding this SoftAP address (network byte
// order) just sent a weather upload. No-op if no tracked client has the
// address. A nonzero gauge_id updates the remembered Rainlog station ID.
void ap_clients_note_upload(uint32_t ip4, uint32_t gauge_id);

// Record that an upload from the device holding this address was accepted by
// Rainlog (possibly minutes after capture, via the retry buffer). Same no-op
// rule as ap_clients_note_upload.
void ap_clients_note_forwarded(uint32_t ip4);

// Same, for a successful relay to Weather Underground.
void ap_clients_note_wu_forwarded(uint32_t ip4);

// Latch AP_CLIENT_ERR_* flags onto the device holding this address. `detail`
// (may be NULL) is the upstream's reject reason; for a *_REJECT flag it is
// stored as the matching rl_reject/wu_reject string (trailing whitespace
// trimmed, truncated to fit).
void ap_clients_note_error(uint32_t ip4, uint8_t flags, const char *detail);

// Clear the given AP_CLIENT_ERR_* flags on the device holding this address
// (the condition resolved: reached after unreachable, accepted after reject).
// Clearing a *_REJECT flag also wipes its stored reason string.
void ap_clients_clear_error(uint32_t ip4, uint8_t flags);

// Set the user-given name for a device, persisted across reboots (NVS, keyed
// by MAC). An empty name removes it.
esp_err_t ap_clients_set_name(const uint8_t mac[6], const char *name);

// Erase every saved device name (factory reset).
void ap_clients_clear_names(void);
