// Rainlog Wireless Bridge - configuration template.
//
// Copy this file to "config.h" (same directory) and fill in your values.
// config.h is gitignored so your WiFi password and Rainlog key never get
// committed. These are only fallback defaults: the SoftAP web configurator
// (see README.md) provisions the real settings at runtime into NVS.

#pragma once

// ---------------------------------------------------------------------------
// WiFi client (STA): the home network the bridge uplinks through to reach the
// internet (Rainlog + the real Weather Underground).
// ---------------------------------------------------------------------------
// Left blank by default: the home network is provisioned at runtime via the web
// configurator, and a blank value lets the setup page show its placeholder
// instead of a dummy SSID.
#define CFG_WIFI_STA_SSID ""
#define CFG_WIFI_STA_PASSWORD ""

// ---------------------------------------------------------------------------
// WiFi host (SoftAP): the network the weather-station console joins. The
// station's DNS + uploads are captured on this side. Password must be >= 8
// chars (WPA2), or empty string for an open AP.
// ---------------------------------------------------------------------------
#define CFG_WIFI_AP_SSID "RainlogBridge"
#define CFG_WIFI_AP_PASSWORD "rainlog123"

// ---------------------------------------------------------------------------
// Hosts / paths (defaults; rarely changed).
//
// NOTE FOR FORKS: these point at rainlog.org, the service this firmware was
// built for. A build with these defaults uploads readings to rainlog.org and
// fetches its firmware updates from rainlog.org. If you are adapting this for
// a different service, repoint CFG_RAINLOG_HOST and CFG_OTA_HOST below.
// ---------------------------------------------------------------------------
// The console is configured with its Rainlog station id ("Rainlog<gaugeId>")
// and the gauge's PWS key directly, so the bridge passes its upload through to
// Rainlog unchanged - no Rainlog credentials are stored here. Weather
// Underground relay credentials are provisioned at runtime as a per-gauge map
// (rainlog gauge id -> WU id + WU key) via the web configurator.
// ---------------------------------------------------------------------------
#define CFG_RAINLOG_HOST "rainlog.org"
#define CFG_WU_HOST "weatherstation.wunderground.com"
#define CFG_WU_UPDATE_PATH "/weatherstation/updateweatherstation.php"

// ---------------------------------------------------------------------------
// OTA firmware updates: a static manifest + image binaries served over HTTPS
// under a path on the main rainlog.org host (we don't control a dedicated
// subdomain's DNS, so the existing nginx serves /rainlog-bridge-ota/; see
// README.md). The bridge checks the manifest shortly after the uplink comes up
// and then every 12-24h, and applies an update on demand from the web
// configurator. The cert validates against the bundled CA roots.
// ---------------------------------------------------------------------------
#define CFG_OTA_HOST "rainlog.org"
// One manifest per board variant, named for its BOARD_ID (board.h) so a 4MB
// and an 8MB build never see each other's images. make-ota.sh writes the
// matching manifest-<BOARD_ID>.json. BOARD_ID is a string literal, so this
// concatenates at the use site (ota_update.c includes board.h).
#define CFG_OTA_MANIFEST_PATH "/rainlog-bridge-ota/manifest-" BOARD_ID ".json"
