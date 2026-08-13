// Rainlog Wireless Bridge - SoftAP config web page.
//
// App-free configurator: the user joins the bridge's SoftAP and browses to the
// SoftAP IP (see wifi_link's WIFI_AP_IP) to set home WiFi + the WU map. Serves
// GET / (the form), GET /config (current values as JSON), and POST /save
// (persist to the config store, then reboot to apply). Registers on the shared
// httpd, so call after capture_server_start().
#pragma once

void config_server_start(void);
