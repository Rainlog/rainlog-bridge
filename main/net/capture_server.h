// Rainlog Wireless Bridge - HTTP capture server.
//
// Listens on port 80 and answers the WU PWS upload endpoint
// (/weatherstation/updateweatherstation.php). The station, DNS-spoofed onto the
// bridge, uploads here (GET query or POST body); we reply "success" right away
// so the station is satisfied, and queue the raw params for the forwarder.
#pragma once

#include "esp_http_server.h"

void capture_server_start(void);

// The shared httpd instance (valid after capture_server_start), so other
// modules (config_server) can register their routes on the same server.
httpd_handle_t capture_server_httpd(void);
