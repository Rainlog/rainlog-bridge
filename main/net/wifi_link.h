// Rainlog Wireless Bridge - WiFi host (SoftAP) + client (STA).
//
// Brings up AP+STA concurrently: the station joins our SoftAP while we uplink
// to the home WiFi. Radio builds can disable interception and run STA-only.
#pragma once

#include <stdbool.h>

#include "esp_netif.h"
#include "esp_wifi.h"

void wifi_link_start(void);

// Most recent cached scan of nearby APs for the config-page picker. The scan
// runs in the background only while the bridge is idle (no phone/station on the
// AP, no uplink), so reading this never blanks the AP. Fills up to `max`
// records, returns the count.
int wifi_link_cached_aps(wifi_ap_record_t *out, int max);

// On-demand LIVE scan (blocks ~1-2s and silences the radio while sweeping
// channels). Used by the config page's manual Scan button to refresh the list,
// even while a phone is connected. Fills up to `max` records, returns the
// count.
int wifi_link_scan_live(wifi_ap_record_t *out, int max);

typedef enum {
  WIFI_TEST_IDLE,
  WIFI_TEST_RUNNING,
  WIFI_TEST_OK,
  WIFI_TEST_FAIL,
} wifi_test_state_t;

// Try connecting to the given home-WiFi creds (config-page Test button).
// Changes the radio channel, so the SoftAP (and any connected phone) briefly
// drops and reconnects. Outcome is polled via wifi_link_test_status(); the
// saved creds are restored in the radio when the test concludes (either way).
// The state stays at the last result until the next test, so the page can read
// it after polling.
void wifi_link_test_start(const char *ssid, const char *pass);
wifi_test_state_t wifi_link_test_status(void);

// Write the SoftAP's dotted IPv4 (the config page / captive-portal address)
// into out. This is the live netif IP, so callers never hardcode the subnet.
void wifi_link_ap_ip_str(char *out, size_t len);

// Write the STA side's dotted IPv4 (the bridge's address on the home LAN)
// into out, or "--" while the uplink is down. Returns true when an IP was
// written.
bool wifi_link_sta_ip_str(char *out, size_t len);

// Info about the home network the STA is currently associated with: its SSID
// (into ssid) and signal strength in dBm (into rssi). Returns false if not
// connected; either out-param may be NULL.
bool wifi_link_sta_ap_info(char *ssid, size_t ssid_len, int *rssi);

// True once the STA side has an IP (uplink usable for forwarding).
bool wifi_link_sta_has_ip(void);

// Number of stations (weather consoles) currently joined to the SoftAP.
int wifi_link_ap_station_count(void);

esp_netif_t *wifi_link_sta_netif(void);
esp_netif_t *wifi_link_ap_netif(void);

// Whether the Wi-Fi driver currently has the bridge AP enabled.
bool wifi_link_ap_enabled(void);
