// Rainlog Wireless Bridge - RGB status LED.
//
// Drives the onboard WS2812. Priority of states, highest first:
//   ERROR        -> solid RED, stays lit until the error clears
//   PROVISIONING -> solid BLUE (unconfigured / BLE provisioning)
//   success flash-> brief GREEN pulse on a successful forward
//   idle         -> off
#pragma once

#include <stdbool.h>

void status_led_init(void);

// Solid red while true; cleared when false. Reflects any error condition
// (failed forward, lost uplink, misconfiguration).
void status_led_set_error(bool on);

// Solid blue while true (device unconfigured / provisioning over BLE).
void status_led_set_provisioning(bool on);

// Brief green pulse to acknowledge a successful Rainlog forward. Suppressed
// while an error is latched (red wins).
void status_led_flash_success(void);
