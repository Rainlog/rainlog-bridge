#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BRIDGE_WIFI_IDLE_US (INT64_C(300) * 1000000)

// Connected consoles count as activity even between their upload intervals.
static inline bool wifi_idle_keep_awake(bool auto_off, bool provisioned,
                                        bool uplink, bool clients, int64_t now,
                                        int64_t last_activity) {
  return !auto_off || !provisioned || !uplink || clients ||
         now - last_activity < BRIDGE_WIFI_IDLE_US;
}
