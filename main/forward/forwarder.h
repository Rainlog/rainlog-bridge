// Rainlog Wireless Bridge - upload forwarder.
//
// Takes encoded radio readings and the raw query string captured from a station's WU upload, re-sends it
// to Rainlog with the Rainlog credentials swapped in, and (optionally) replays
// it to the real Weather Underground with the configured WU credentials.
// Forwarding runs on its own task so the capture handler can answer the
// station immediately instead of blocking on TLS for up to 30s.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Create the forward queue + worker task. Call once at boot, before
// capture_server_start.
void forwarder_init(void);

// Queue one captured upload for forwarding. `raw_query` is the URL query
// string exactly as the station sent it (no leading '?'), e.g.
// "ID=KXXX&PASSWORD=abc&dateutc=now&rainin=0.04&tempf=70"; `src_ip` is the
// uploading station's SoftAP address (network byte order, 0 = unknown), used
// to credit the per-client forwarded count once Rainlog accepts. Copies the
// string; returns false if the queue is full (upload dropped) or on alloc
// failure. Radio-reserved gauges reject Wi-Fi submission.
bool forwarder_submit(const char *raw_query, uint32_t src_ip);

// Short status of the most recent Rainlog forward, for the UI:
// "--" (none yet), "OK", "REJECT" (200 but not accepted), "FAIL" (no 200).
const char *forwarder_last_result(void);

// Seconds since the most recent forward attempt (monotonic, immune to SNTP
// clock jumps), or -1 if none yet.
int64_t forwarder_seconds_since_upload(void);

// Number of captured uploads currently buffered for retry (failed to fully
// deliver to Rainlog and/or WU). 0 when everything has been forwarded.
int forwarder_pending_count(void);
