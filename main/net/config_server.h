// Rainlog Wireless Bridge - SoftAP and home-LAN config web page.
//
// App-free configurator: the user joins the bridge's SoftAP and browses to the
// SoftAP or home-LAN IP to set Wi-Fi, WU relays and radio mappings. Serves
// GET / (the form), GET /config (current values as JSON), and POST /save
// (persist to the config store, then reboot to apply). Registers on the shared
// httpd, so call after capture_server_start().
#pragma once

void config_server_start(void);

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
// Shared management operations. Returned JSON is malloc-owned by caller.
char *config_server_config_json(void);
char *config_server_scan_json(bool live);
char *config_server_clients_json(uint32_t peer_ip);
esp_err_t config_server_save_form(const char *body, const char **error);
esp_err_t config_server_rename(const char *mac, const char *name);
void config_server_restart(void);

#if RAINLOG_RADIO
char *config_server_radio_json(void);
#endif
