# Rainlog Bridge config web UI

The setup page (TypeScript + CSS + favicon) is built into one self-contained,
minified page per variant: `../main/web/index.html` for radio-disabled firmware
and `../main/web/radio433/index.html` for 433 MHz radio firmware. Each firmware
build embeds its selected page and serves it at `/`.

## Build

```sh
npm install      # once
npm run build    # regenerates both embedded pages
npm run typecheck
```

`build.mjs` bundles + minifies `src/app.ts` (esbuild), minifies `src/style.css`,
inlines the favicon as a `data:` URI, splices all three into `src/index.html`,
and minifies the document. It then writes `index.html.gz` beside each HTML
page using Zopfli with 100 iterations and checks decompression matches exactly.
The firmware embeds the gzip file and serves it with `Content-Encoding: gzip`.
Compression runs only during the Node build, not on the ESP32.

## Why a single static file

The firmware serves `/` as static bytes (no server-side templating). The page
fetches current settings from the firmware's `/config` JSON endpoint at load and
fills the form client-side. Wi-Fi passwords are not returned. Authenticated requests receive saved WU
keys and radio Rainlog PWS keys for editing behind Show/Hide controls.

Both generated pages and their gzip files are **committed**: the firmware build runs in the
pinned ESP-IDF Docker image, which has no Node. Re-run `npm run build` and commit
the regenerated files whenever you change anything under `src/`.

## Devices

Devices lists Wi-Fi clients while interception is active and radio sensors on
radio builds. The Weather Underground mapper lives here. Its editable station
field offers IDs from saved mappings and captured Wi-Fi uploads; a single
unassigned known ID fills a single empty row automatically. Both tabs' save
buttons submit the shared configuration.

## Radio setup

Setup offers Wi-Fi interception and radio reception switches, with reception
status and the number of devices seen. Devices displays full-width sensor cards with mapping controls
inside each card, saving up to eight sensor-to-Rainlog mappings.
Each mapping includes model, decimal sensor ID, Iris channel where applicable,
Rainlog station ID and PWS key. Add mapping on a detected sensor card supplies its model, ID and channel
automatically. Saved mappings remain intact when a sensor
has not been heard since reboot. Bridge Wi-Fi fields and their password toggle are disabled while interception
is unchecked; saved credentials are retained. Settings apply after reboot.
Rainfall forwarding from those mappings is still pending.

`tests/test-radio-web.cjs` exercises the generated pages in Chromium with
fixture API responses. It requires Playwright; set `PLAYWRIGHT_MODULE` to an
installed module path when it is not in the default Node resolution path.
`tests/test-radio-settings.py SERIAL_DEVICE` tests NVS persistence and STA-only
LAN setup on a configured debug build, then restores original settings.

Saved TX5U mappings offer Replace sensor only when another seen TX5U is
available and not already assigned to another mapping. Replacement retains the Rainlog station ID and
PWS key, and applies on Save and reboot. While reception is active, a TX5U
not seen this boot or silent for five minutes is flagged. Iris mappings do
not get this replacement action.

Sensor cards embed original matching SVG drawings from `src/icons/`: a
tipping-bucket collector for TX5U and a weather-station mast for Iris. Esbuild
loads these as text, so no separate asset requests or icon library are needed.

At least one reception source must remain enabled: the last enabled checkbox
cannot be unchecked. Firmware validation also rejects both sources off,
including console settings. Radio-free builds omit both reception switches
and always use Wi-Fi interception. Older saved configurations with both off
resume radio reception on boot.

## Manual firmware update

Firmware offers a Manual update section for a trusted app `.bin` for this exact
board. The browser previews version, board and size; the device repeats the
checks. Files need the fixed-offset Rainlog board descriptor added by current
builds, so rebuild older images first. Merged flash images and bootloaders are
rejected. Manual installs can use the same version or downgrade.

The authenticated binary POST `/ota/upload` requires `X-Rainlog-OTA: 1` and
`Content-Type: application/octet-stream`. Streaming uses a 1 KiB buffer, writes
only the inactive OTA slot, and rejects interrupted uploads, wrong identity,
slot overflow, missing SHA-256 hashes, corrupt images and trailing data. A
shared operation lock excludes concurrent manual/server updates. Only successful
ESP-IDF image validation selects the boot slot and schedules reboot. The existing
20-second boot health check cancels automatic rollback after startup succeeds.
Browser cancellation is offered while bytes are uploading, before validation.

`tests/test-firmware-image.sh` checks header and identity rejection. Optional
arguments validate the real LILYGO app image and reject the real C6 image.
`tests/test-manual-ota.py SERIAL LILYGO_APP_BIN C6_APP_BIN` performs destructive
OTA-slot tests only on the selected LILYGO debug board, ending with a valid
manual install and boot health check. It retains provisioning and mappings.

Setup, Devices and Firmware use `/setup`, `/devices` and `/firmware` routes.
Tab navigation preserves unsaved fields and supports browser history and direct
links. The firmware serves the authenticated app shell at each route; `/` opens
Setup.
