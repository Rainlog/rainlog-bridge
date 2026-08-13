// Rainlog Wireless Bridge - SNTP wall-clock sync.
//
// Wall clock is needed for the rolling 24h stats window and for stamping
// uploads. Safe to start before the uplink is up; SNTP retries once online.
#pragma once

void time_sync_start(void);
