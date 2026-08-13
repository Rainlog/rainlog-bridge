// Rainlog Wireless Bridge - DNS server (spoof + proxy).
//
// Runs on the SoftAP side. Answers *.wunderground.com with the bridge's own AP
// IP so the station's WU upload lands on our capture server; every other lookup
// is proxied to the uplink resolver so the station keeps normal name
// resolution.
#pragma once

void dns_server_start(void);
