// Rainlog Wireless Bridge - on-device filesystem (LittleFS on the `storage`
// partition). Power-loss resilient and wear-leveled, so it can absorb frequent
// small writes (the upload-stats file) that would wear out the tiny NVS
// partition. Mounted once at boot; consumers use stdio under FS_BASE.
#pragma once

#include <stdbool.h>

// Mount point for the LittleFS `storage` partition.
#define FS_BASE "/storage"

// Mount LittleFS on the `storage` partition at FS_BASE, formatting it on first
// boot or if the existing image is unmountable. Call once at boot, before any
// consumer reads its files. Returns true on success; on failure the device
// runs without persistence (callers must tolerate file ops failing).
bool fs_mount(void);
