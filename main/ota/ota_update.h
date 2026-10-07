// Rainlog Wireless Bridge - over-the-air firmware updates.
//
// Checks an HTTPS manifest (CFG_OTA_HOST/CFG_OTA_MANIFEST_PATH) for a firmware
// version newer than the running one, and applies it with esp_https_ota into
// the inactive OTA slot. The downloaded image is checked against the manifest's
// sha256 before it is committed. A freshly flashed image must then pass a
// health check and call ota_update_mark_valid() or the bootloader rolls it back
// on the next reset.
//
// The check runs automatically shortly after the uplink first comes up and then
// every 12-24h (randomized). The actual "update now" trigger lives in the web
// configurator: it pokes ota_update_request_apply(). The status screen shows a
// "FW update" line whenever ota_update_available() is true.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Start the background OTA task (checker + applier). Call once at boot, after
// WiFi is up. Idempotent guards inside; safe even if the uplink is still down.
void ota_update_start(void);

// True if the last successful manifest check found a newer firmware version.
// Read by the LCD to show the update notice; never blocks on the network.
bool ota_update_available(void);

// Copy the latest version string from the most recent manifest check into out
// (empty if no check has succeeded). For the LCD update notice.
void ota_update_latest(char *out, size_t len);

// Ask the background task to fetch the manifest now (web "Check for updates").
// Non-blocking; poll ota_update_status_json() for the outcome.
void ota_update_request_check(void);

// Ask the background task to download + apply the newest firmware now (web
// "Update now"). Non-blocking; on success the device reboots into the new
// image. Poll ota_update_status_json() for progress.
void ota_update_request_apply(void);

#define OTA_STATUS_JSON_MAX 512

// Write the current OTA status as a JSON object into out:
//   {"phase":"idle|checking|uptodate|available|updating|error",
//    "running":"<ver>","latest":"<ver>","available":bool,
//    "progress":<0-100>,"error":"<msg>","board":"<board>",
//    "max_image_size":<bytes>,"running_slot":"<slot>",
//    "boot_slot":"<slot>","pending_verification":bool}
void ota_update_status_json(char *out, size_t len);

// If the running image is pending verification after an OTA, mark it valid so
// the bootloader won't roll it back. Call once the bridge has proven healthy
// (servers up, ran a while without crashing). A no-op otherwise.
void ota_update_mark_valid(void);

// Manual uploads share an exclusive operation lock with server OTA updates.
// Only a fully verified image is selected for boot. Finish success retains
// the lock until the caller schedules a reboot; failures keep the old image.
typedef struct ota_manual ota_manual_t;
esp_err_t ota_manual_begin(const uint8_t *prefix, size_t prefix_size,
                           size_t image_size, ota_manual_t **upload,
                           const char **error);
esp_err_t ota_manual_write(ota_manual_t *upload, const uint8_t *data, size_t len);
esp_err_t ota_manual_finish(ota_manual_t *upload, const char **error);
void ota_manual_abort(ota_manual_t *upload);
