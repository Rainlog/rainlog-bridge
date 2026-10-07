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
and minifies the document.

## Why a single static file

The firmware serves `/` as static bytes (no server-side templating). The page
fetches current settings from the firmware's `/config` JSON endpoint at load and
fills the form client-side. Wi-Fi passwords are not returned. Authenticated requests receive saved WU
keys and radio Rainlog PWS keys for editing behind Show/Hide controls.

Both generated pages are **committed**: the firmware build runs in the
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
