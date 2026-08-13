# Rainlog Wireless Bridge

ESP32-C6 firmware that lets an existing personal weather station report rain to
[Rainlog.org](https://rainlog.org) without replacing the station or reconfiguring
anything on it permanently.

Most consumer weather station consoles can upload to Weather Underground and
nowhere else. This device stands in for Weather Underground: the console
uploads to the bridge believing it is talking to WU, and the bridge forwards
the reading to Rainlog, then relays it on to the real Weather Underground so
the owner keeps their existing WU station working.

The console needs no firmware change and no vendor cooperation. It only needs
to be pointed at the bridge's WiFi network.

## How it works

The bridge runs SoftAP and station mode on one radio at the same time:

- **SoftAP side** (`10.41.0.1/24`, WPA2): the weather station console joins
  this. The subnet is deliberately off the common home ranges so it cannot
  collide with the uplink side.
- **Station side**: the bridge joins the home WiFi for its own internet access.

The console's uploads are captured by spoofing WU on the SoftAP side:

1. `net/dns_server.c` (UDP :53) answers `*.wunderground.com` with the bridge's
   own AP address, and proxies every other name to the real upstream resolver.
   While the bridge is still unprovisioned it answers *all* names with the AP
   address, making a full captive portal.
2. `net/capture_server.c` serves `/weatherstation/updateweatherstation.php` on
   both port 80 and port 443. Some consoles (AcuRite among them) insist on TLS,
   so there is an HTTPS listener with a self-signed certificate for the spoofed
   hostname. See [`main/certs/README.md`](main/certs/README.md) for why a
   private key is committed to this repository on purpose.
3. It answers `success` immediately, then queues the raw query for the
   forwarder, so a slow uplink never stalls the console.
4. `forward/forwarder.c` sends the reading to Rainlog unchanged, and, if a WU
   mapping is configured for that gauge, also relays it to the real WU with the
   station id and password rewritten.

The bridge also does real NAT for its SoftAP clients, so the console gets
genuine internet access for NTP and vendor cloud services. Some consoles (again
AcuRite) refuse to upload at all until their clock syncs.

**Readings are not dropped on a flaky uplink.** The forwarder keeps a
store-and-forward retry buffer mirrored to flash, so a reboot, a firmware
update, or a crash does not lose an undelivered reading. It drains newest-first,
because the WU protocol's rain fields are cumulative: the freshest reading
already carries the full total.

## Hardware

**Waveshare ESP32-C6-LCD-1.47.** RISC-V single core, WiFi 6, with a 1.47 inch
172x320 ST7789 LCD on board.

The units this targets are **ESP32-C6FH8 with 8MB embedded flash**, despite
the Waveshare spec and demo claiming 4MB. Verify yours with `esptool flash-id`.

Flash size fixes the partition layout, and the layouts are not interchangeable,
so there is **one image stream per flash variant**. The size is encoded in
`BOARD_ID` (`main/board.h`), which also derives the OTA manifest filename, so a
4MB build and an 8MB build can never fetch or apply each other's images.

| Variant | `BOARD_ID` | Partitions | Status |
|---|---|---|---|
| C6FH8 (8MB) | `esp32-c6fh8-lcd-1.47` | `partitions.csv` | Built and shipping |
| C6FH4 (4MB) | `esp32-c6fh4-lcd-1.47` | `partitions-c6fh4.csv` | Staged, untested (no hardware on hand) |

### GPIO map

Shared SPI bus (LCD and SD): SCLK 7, MOSI 6, MISO 5. The single source of truth
in code is `main/board.h`.

| Function | GPIO |
|---|---|
| LCD CS / DC / RST | 14 / 15 / 21 |
| LCD backlight | 22 |
| SD CS | 4 |
| RGB LED (WS2812) | 8 |
| BOOT button | 9 |

Note the onboard WS2812 is **RGB** wire order on this board, not the usual GRB.

## Building

Native ESP-IDF, not Arduino. **Every build runs inside Docker**, against a
pinned `espressif/idf` image, so no host toolchain is needed or assumed.

```sh
cp main/config.example.h main/config.h   # required once, before the first build
./build.sh                               # build
./build.sh flash                         # build, flash, and open the serial monitor
```

`config.h` is gitignored. It holds only compile-time fallbacks; everything
user-facing is provisioned at runtime through the web configurator. **Never
commit WiFi passwords or PWS keys.**

Flashing defaults to `/dev/ttyACM0` (the board enumerates as native USB CDC).
Override with `PORT=/dev/ttyXXX ./build.sh flash`.

The target (`esp32c6`) is pinned in `sdkconfig.defaults`. The generated
`sdkconfig` and `build/` are gitignored.

### Vendored dependencies

`managed_components/` is **committed, not fetched**, and `dependencies.lock`
pins exact versions and hashes. This is deliberate: the Docker build never
needs network access, so a build is reproducible and cannot break because an
upstream registry moved.

### The configuration web UI

The setup page is a small TypeScript + esbuild subproject under `web/`. It
builds into a single self-contained `main/web/index.html` (CSS, JS, and favicon
inlined and minified) that the firmware embeds.

```sh
cd web && npm install && npm run build
```

**The generated `main/web/index.html` must be committed.** The ESP-IDF Docker
image has no Node, so the firmware build never runs npm. Edit sources under
`web/src/`, rebuild, and commit the regenerated file. `web/node_modules` is
gitignored.

## First run

1. Flash and power on. The LCD shows the setup screen: the bridge's own WiFi
   name, its password, and the setup address.
2. Join that WiFi from a phone or laptop. The captive portal should open by
   itself; if not, browse to `http://10.41.0.1`.
3. Enter the home WiFi credentials (there is a scan list, and a test button
   that tries them without saving), and set the gauge mapping. The Rainlog
   station field accepts either `Rainlog<gaugeId>` or the bare gauge number.
4. Save. The bridge reboots and comes up on the status screen.
5. Point the weather station console at the bridge's WiFi, configured to upload
   to Weather Underground as usual with station id `Rainlog<gaugeId>` and the
   gauge's Rainlog PWS key as the password.

The configurator is reachable afterwards from the home network too, at the
bridge's home IP shown on its screen, gated by a sign-in using the bridge's own
WiFi password.

### Controls and indicators

- **BOOT tap**: wakes the backlight (it dims after 30 seconds idle).
- **BOOT held ~11 seconds**: factory reset, wiping configuration, device names,
  and stored statistics.
- **RGB LED**: solid red is an error, a green pulse is a successful forward,
  blue means unprovisioned or connecting.

## Firmware updates (OTA)

The bridge checks for updates shortly after its uplink comes up, then on a
randomized 12 to 24 hour interval. Updates stream into the inactive OTA slot,
are verified against the manifest's sha256 over the written partition before
commit, and reboot. Rollback is enabled: the image is only marked valid about
20 seconds after a successful boot, so an update that crash-loops reverts
itself.

To cut a release:

1. Bump `PROJECT_VER` in `CMakeLists.txt`. It is the single source of truth,
   baked into `esp_app_desc` and shown on the LCD.
2. Run `./make-ota.sh`, which builds in Docker and writes
   `dist/rainlog-bridge-<board>-<version>.bin` plus `manifest-<board>.json`.
3. Publish by rsyncing `dist/` to the directory the web server exposes at
   `/rainlog-bridge-ota/`.

Filenames are versioned and immutable, so old images can stay for reference.
The firmware fetches `manifest-<its own BOARD_ID>.json` and rejects a manifest
whose `board` field does not match its own.

Scope limits worth stating plainly: TLS to the update host validates against
the bundled CA roots, but certificate *expiry* is not checked, because the
build omits `MBEDTLS_HAVE_TIME_DATE`. Update integrity therefore rests on the
manifest sha256 verified in firmware before commit. Secure boot, flash
encryption, and anti-rollback are deliberately out of scope, since none of them
are useful without burning eFuses.

## Reference: the Waveshare demo

`./fetch-demo.sh` downloads and extracts the official Waveshare demo archive
(about 60MB) into `ESP32-C6-LCD-1.47-Demo/`, which is gitignored. It is the
reference for board bring-up: working init sequences and register flows for the
ST7789 LCD, the WS2812 LED, the SPI bus, and the SD card. Consult it before
writing peripheral code from scratch.

## Security notes

- The two network sides are **not routed together** for the capture path. The
  upload endpoint is gated to SoftAP-side requests, so nothing on the home LAN
  or the internet can spoof a reading into it.
- The configurator uses one secret, the bridge's own WiFi password. On the LAN
  side it requires a sign-in that sets a RAM-only session cookie, so a reboot
  signs everyone out. Failed logins are throttled. HTTP Basic auth with the
  same password is accepted for scripting, but no `WWW-Authenticate` header is
  ever sent, so browsers never pop a native dialog.
- Secrets are never echoed back by the config API, only reported as
  is-set booleans.
- **A private key is committed** in `main/certs/`, on purpose. Read
  [`main/certs/README.md`](main/certs/README.md) before concluding otherwise.

## Licensing

The firmware in this repository is MIT licensed. See [LICENSE](LICENSE).

Three carve-outs, none of which MIT covers:

- `managed_components/joltwallet__littlefs/` is vendored third-party code under
  its own terms. See the `LICENSE` files inside that directory.
- `main/net/oui_table.h` is generated from the public IEEE OUI registry by
  `tools/gen-oui.py`.
- The **Rainlog name and logo** (`main/ui/header_logo.h`,
  `web/src/favicon.png`) are branding, not covered by the code license.

Note also that a default build uploads readings to, and fetches firmware
updates from, `rainlog.org`. A fork intended for another service should
repoint `CFG_RAINLOG_HOST` and `CFG_OTA_HOST` in `main/config.h`.
