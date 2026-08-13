// Rainlog Wireless Bridge - processed-message stats.
//
// Tracks uploads successfully forwarded to each upstream target (Rainlog
// always, the optional Weather Underground relay when configured) so the UI
// can show rolling 24h counts plus lifetime totals. These live in RAM and
// reset on reboot. Optional NVS persistence (the totals restored at boot, the
// 24h windows re-anchored after SNTP sync) is compiled out by default to
// avoid flash wear; enable with -DSTATS_PERSIST=1 (see upload_stats.c).
//
// Symbols are prefixed `upload_stats_` rather than `stats_` because lwip
// defines a `stats_init()` macro (lwip/stats.h), pulled in transitively by the
// socket headers, that would otherwise clash.
#pragma once

#include <stdint.h>

// Upstream targets tracked separately.
typedef enum {
  UPLOAD_TARGET_RL = 0,  // Rainlog (the headline metric)
  UPLOAD_TARGET_WU = 1,  // Weather Underground relay
  UPLOAD_TARGET_COUNT
} upload_target_t;

// Create the lock and load persisted stats. Call once at boot, after NVS init.
void upload_stats_init(void);

// Record one upload that was successfully accepted by the given target
// (timestamped with monotonic esp_timer time, immune to SNTP clock jumps) and
// persist.
void upload_stats_record_success(upload_target_t target);

// Notify that the wall clock is now valid (SNTP synced). Re-anchors any ring
// entries persisted before the last power-off onto the monotonic timeline so
// the 24h counts survive a reboot. Safe to call repeatedly; only the first
// call after boot does work.
void upload_stats_clock_synced(void);

// Count successful forwards to the target in the trailing 24h window.
int upload_stats_count_last_24h(upload_target_t target);

// Lifetime total of successful forwards to the target (persists across
// reboots).
uint32_t upload_stats_total(upload_target_t target);

// Erase persisted stats (factory reset). Caller typically reboots right after.
void upload_stats_clear(void);
