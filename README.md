# Rainlog Wireless Bridge

ESP32-C6 firmware that lets an existing personal weather station report rain to
[Rainlog.org](https://rainlog.org) without replacing the station.

Most consumer weather station consoles can upload to Weather Underground and
nowhere else. This device stands in for Weather Underground: the console
uploads to the bridge believing it is talking to WU, and the bridge forwards
the reading to Rainlog, then relays it on to the real Weather Underground so
your existing WU station keeps working.

Your console needs no firmware change and no vendor cooperation. It only needs
to be pointed at the bridge's WiFi.

## Setting up your bridge

**1. Get your station credentials from Rainlog.**

On [rainlog.org](https://rainlog.org), set your gauge's reporting mode to
**Automatic**, then open the gauge and click **View station credentials**.
Keep the **Weather Underground** tab selected and note two values:

- **Station ID**, which looks like `Rainlog12345`
- **Station key**

**2. Power on the bridge.**

The screen shows a setup message with the bridge's own WiFi name and password.

**3. Connect the bridge to your home WiFi.**

Join the bridge's WiFi from a phone or laptop. A setup page should open by
itself; if it does not, browse to `http://10.41.0.1`.

Pick your home network from the list and enter its password. There is a Test
button that checks the password before you commit to it. Save, and the bridge
restarts and shows its status screen.

While you are here, set a new password for the bridge's own WiFi. It must be at
least 8 characters, and the default is rejected.

**4. Point your weather station at the bridge.**

In your console's settings, find where it uploads to Weather Underground and
set:

- **Station ID**: the `Rainlog12345` value from step 1
- **Station key / password**: the station key from step 1

Then join the console to the **bridge's** WiFi instead of your home WiFi.

Leave the protocol set to Weather Underground. Some consoles offer an
"Ecowitt" or "Customized" mode; those upload a different way and the bridge
will not see them.

**5. Check that it works.**

Wait for your console's next upload (typically well under 5 minutes). The
bridge's screen shows a Forwarding count that ticks up, and the LED pulses
green on each successful forward. Your readings then appear on rainlog.org.

If you also want to keep uploading to Weather Underground, enter your WU
station ID and key on the bridge's setup page and it will relay there too.

### If something is wrong

The bridge's screen lists your home WiFi, its own WiFi, and per-device counts,
which is usually enough to tell where a problem is. The setup page's Devices
tab shows each connected station with any error reason.

- **LED solid red**: an error. Check the screen.
- **LED blue**: not yet set up, or still connecting.
- **Nothing forwarding**: confirm the console joined the *bridge's* WiFi and
  that its protocol is Weather Underground, not Ecowitt.

You can reach the setup page later from your home network too. Browse to the
bridge's home IP shown on its screen and sign in with the bridge's WiFi
password.

### Buttons

- **BOOT tap**: wakes the backlight, which dims after 30 seconds idle.
- **BOOT held about 11 seconds**: factory reset. Wipes configuration, device
  names, and stored statistics.

## How it works

The bridge runs its own access point and joins your home WiFi at the same time,
on one radio.

- **Bridge side** (`10.41.0.1/24`, WPA2): your console joins this. The subnet is
  deliberately off the common home ranges so it cannot collide with your LAN.
- **Home side**: the bridge's uplink to the internet.

Uploads are captured by impersonating Weather Underground on the bridge side:

1. `net/dns_server.c` (UDP :53) answers `*.wunderground.com` with the bridge's
   own address and proxies everything else to the real resolver. While
   unprovisioned it answers all names, making a full captive portal.
2. `net/capture_server.c` serves `/weatherstation/updateweatherstation.php` on
   ports 80 and 443. Some consoles (AcuRite) insist on TLS, hence the HTTPS
   listener with a self-signed certificate. See
   [`main/certs/README.md`](main/certs/README.md) for why a private key is
   committed here on purpose.
3. It answers `success` immediately and queues the raw query, so a slow uplink
   never stalls the console.
4. `forward/forwarder.c` sends the reading to Rainlog unchanged, and relays to
   the real WU with the ID and password rewritten when a mapping is set.

The bridge also NATs for its clients, so the console gets real internet for NTP
and vendor services. Some consoles refuse to upload until their clock syncs.

Readings survive a flaky uplink: the forwarder keeps a store-and-forward retry
buffer mirrored to flash, so a reboot, an update, or a crash does not drop an
undelivered reading. It drains newest-first, because the WU protocol's rain
fields are cumulative and the freshest reading already carries the full total.

## Hardware

**Waveshare ESP32-C6-LCD-1.47.** RISC-V single core, WiFi 6, 1.47 inch 172x320
ST7789 LCD.

These units are **ESP32-C6FH8 with 8MB flash**, despite the Waveshare spec and
demo claiming 4MB. Check yours with `esptool flash-id`.

Flash size fixes the partition layout and the layouts are not interchangeable,
so there is one image stream per variant, keyed on `BOARD_ID` (`main/board.h`).

| Variant | `BOARD_ID` | Partitions | Status |
|---|---|---|---|
| C6FH8 (8MB) | `esp32-c6fh8-lcd-1.47` | `partitions.csv` | Built and shipping |
| C6FH4 (4MB) | `esp32-c6fh4-lcd-1.47` | `partitions-c6fh4.csv` | Staged, untested (no hardware on hand) |

The 4MB layout has no `storage` partition, so the retry buffer and statistics
would not persist across reboots on that variant.

### GPIO map

Shared SPI bus (LCD and SD): SCLK 7, MOSI 6, MISO 5. Single source of truth is
`main/board.h`.

| Function | GPIO |
|---|---|
| LCD CS / DC / RST | 14 / 15 / 21 |
| LCD backlight | 22 |
| SD CS | 4 |
| RGB LED (WS2812) | 8 |
| BOOT button | 9 |

The onboard WS2812 is **RGB** wire order on this board, not the usual GRB.

## Building

Native ESP-IDF, not Arduino. Every build runs inside Docker against a pinned
`espressif/idf` image, so no host toolchain is needed.

```sh
cp main/config.example.h main/config.h   # once, before the first build
./build.sh                               # build
./build.sh flash                         # build, flash, serial monitor
```

`config.h` is gitignored and holds only compile-time fallbacks; everything
user-facing is provisioned at runtime. Never commit WiFi passwords or PWS keys.

Flashing defaults to `/dev/ttyACM0`; override with `PORT=/dev/ttyXXX`.
`./build.sh flash` writes the bootloader, partition table, OTA data, and app.

`managed_components/` is committed rather than fetched, and `dependencies.lock`
pins exact versions, so the build never needs network access.

### Configuration web UI

The setup page is a TypeScript + esbuild subproject in `web/`, built into a
single self-contained `main/web/index.html` that the firmware embeds.

```sh
cd web && npm install && npm run build
```

**The generated `main/web/index.html` must be committed**: the IDF image has no
Node, so the firmware build never runs npm. CI fails if it is stale.

### Reference drivers

`./fetch-demo.sh` downloads the official Waveshare demo (about 60MB, gitignored)
for working ST7789, WS2812, SPI, and SD init sequences.

## Firmware updates (OTA)

The bridge checks shortly after its uplink comes up, then every 12 to 24 hours
(randomized). Updates stream into the inactive slot, are verified against the
manifest's sha256 before commit, and reboot. Rollback is enabled: the image is
marked valid only about 20 seconds after a good boot, so an update that
crash-loops reverts itself.

To cut a release:

1. Bump `PROJECT_VER` in `CMakeLists.txt`. It is baked into `esp_app_desc` and
   shown on the LCD.
2. `./make-ota.sh` builds and writes `dist/rainlog-bridge-<board>-<version>.bin`
   plus `manifest-<board>.json`.
3. rsync `dist/` to the document root served at `/rainlog-bridge-ota/`.

Filenames are versioned and immutable. The firmware fetches
`manifest-<its own BOARD_ID>.json` and rejects a manifest whose `board` does
not match.

**OTA replaces the app only.** The bootloader and partition table are not
touched, so a partition layout change cannot ship over the air and requires a
USB reflash.

Scope limits, stated plainly: TLS to the update host validates against the
bundled CA roots, but certificate *expiry* is not checked, because the build
omits `MBEDTLS_HAVE_TIME_DATE`. Update integrity rests on the manifest sha256
verified before commit. Secure boot, flash encryption, and anti-rollback are
deliberately out of scope, since none are useful without burning eFuses.

## Security notes

- The two network sides are not routed together for the capture path. The
  upload endpoint is gated to bridge-side requests, so nothing on the home LAN
  or the internet can inject a reading.
- The configurator uses one secret, the bridge's own WiFi password. From the
  LAN it requires a sign-in that sets a RAM-only session cookie, so a reboot
  signs everyone out. Failed logins are throttled. HTTP Basic auth with the
  same password works for scripting, but no `WWW-Authenticate` is ever sent, so
  browsers never pop a native dialog.
- WiFi passwords are never returned by the config API. **The per-gauge WU
  upload keys are**, deliberately: `GET /config` includes them so the page can
  populate the form, shown behind a Show toggle. They are per-station upload
  keys rather than network credentials, and the route is already gated.
- **A private key is committed** in `main/certs/`, on purpose. Read
  [`main/certs/README.md`](main/certs/README.md) before concluding otherwise.

## Licensing

MIT, see [LICENSE](LICENSE). Three carve-outs it does not cover:

- `managed_components/joltwallet__littlefs/` is vendored third-party code under
  its own terms.
- `main/net/oui_table.h` is generated from the public IEEE OUI registry.
- The **Rainlog name and logo** (`main/ui/header_logo.h`, `tools/icon-512.png`,
  `web/src/favicon.png`) are branding.

A default build uploads to, and fetches updates from, `rainlog.org`. A fork for
another service should repoint `CFG_RAINLOG_HOST` and `CFG_OTA_HOST` in
`main/config.example.h`.
