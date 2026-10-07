# Rainlog Wireless Bridge

ESP32-C6 firmware that lets an existing personal weather station report rain to
[Rainlog.org](https://rainlog.org) without replacing the station.

Some consoles can only upload to Weather Underground, with no way to enter a
custom server. This device stands in for Weather Underground: the console
uploads to the bridge believing it is talking to WU, and the bridge forwards
the reading to Rainlog. It can also relay to the real Weather Underground, so
an existing WU station keeps working, once you configure that.

Your console needs no firmware change and no vendor cooperation. It only needs
to be pointed at the bridge's WiFi.

## Do you need one?

**Probably not.** If your station can upload to a custom server (usually a
"Customized" upload setting where you can type a hostname), point it straight
at rainlog.org instead. That is simpler, has nothing extra to power, and is
what Rainlog's own [PWS setup guide](https://rainlog.org/help/pws-setup)
recommends. Rainlog accepts both the Weather Underground and Ecowitt upload
protocols directly.

This bridge is for stations that **cannot** enter a custom server at all, such
as the AcuRite Iris, where uploading to Weather Underground is the only option
the console offers.

It is also still in beta. If you want to try one, email Rainlog first.

## Setting up your bridge

**1. Get your station credentials from Rainlog.**

On [rainlog.org](https://rainlog.org), go to **Data > View/Edit Gauges**, edit
your gauge, set its reporting mode to **Automatic**, and **save**. The next
step does not appear until you have saved.

Back on the gauges list, the gauge now shows a small **key icon**. Click it.
With **Weather Underground** selected, note two values:

- **Station ID**, which looks like `Rainlog12345`
- **Station Key**

**2. Power on the bridge.**

The screen shows a setup message with the bridge's own WiFi name and password.

**3. Connect the bridge to your home WiFi.**

Join the bridge's WiFi from a phone or laptop. A setup page should open by
itself; if it does not, browse to `http://10.41.0.1`.

Pick your home network from the list and enter its password. **Scan networks**
refreshes the list if it is empty, and you can also just type the name.
**Test connection** checks the password before you commit to it.

You must also **set a new password for the bridge's own WiFi**. This is
required, not optional: it must be at least 8 characters, the default is
rejected, and the bridge will not save until you change it. **Write it down**,
because you need it in the next step and the status screen never shows it
again.

Then **Save & reboot bridge**. The bridge restarts and shows its status
screen.

**4. Point your weather station at the bridge.**

In your console's settings, find where it uploads to Weather Underground and
set:

- **Station ID**: the `Rainlog12345` value from step 1
- **Station key / password**: the station key from step 1

Then join the console to the **bridge's** WiFi instead of your home WiFi.

Set the **protocol to Weather Underground** (sometimes called Wunderground).
If your console calls this a "Customized" upload, that is fine and expected,
just keep the protocol set to Weather Underground.

Do **not** select the Ecowitt protocol. The bridge only captures Weather
Underground uploads. (Rainlog itself accepts Ecowitt, but only for stations
uploading to it directly, which do not need a bridge.)

**5. Check that it works.**

Wait for your console's next upload. The bridge's screen has a **Forwarding**
section whose count rises, and the LED pulses green on each forward. Your
readings then appear on rainlog.org.

Do not expect that count to move on every upload. Rainlog accepts one reading
per gauge per 5 minutes, so the bridge throttles undated uploads to match. A
console that reports every 18 seconds will show its received count climbing
steadily while the forwarded count rises only once per 5 minutes. That is
working correctly.

To keep uploading to Weather Underground as well, add a relay row under **Devices**.
The station field offers known Rainlog IDs and also accepts manual entry.
It needs **three** values: your Rainlog station ID (`Rainlog12345`), your
WU station ID, and your WU key. A row missing any of them is silently ignored,
so double check all three.

### If something is wrong

The bridge's screen lists your home WiFi, its own WiFi, and per-device counts,
which is usually enough to tell where a problem is. The setup page's Devices
tab shows each connected station with any error reason.

- **LED dark**: normal. The LED is off during healthy operation and only
  pulses green on a forward.
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
  deliberately off the common home ranges so an overlap with your LAN is very
  unlikely. Nothing detects one, so a home LAN actually on `10.41.0.0/24` would
  misroute the bridge's uplink.
- **Home side**: the bridge's uplink to the internet.

Uploads are captured by impersonating Weather Underground on the bridge side:

1. `net/dns_server.c` (UDP :53) answers `*.wunderground.com` with the bridge's
   own address, and does the same for a fixed list of OS connectivity-check
   hosts so the setup page pops up as a captive portal. That list stays active
   even once provisioned. Everything else is proxied to the real resolver,
   except while unprovisioned, when it answers all names.
2. `net/capture_server.c` serves `/weatherstation/updateweatherstation.php` on
   ports 80 and 443. Some consoles (AcuRite) insist on TLS, hence the HTTPS
   listener with a self-signed certificate. See
   [`main/certs/README.md`](main/certs/README.md) for why a private key is
   committed here on purpose.
3. It answers `success` immediately and queues the raw query, so a slow uplink
   never stalls the console.
4. `forward/forwarder.c` sends the reading to Rainlog, appending an
   `rlbridge=<version>` marker, and relays to the real WU with the ID and
   password rewritten when a mapping is configured. Readings timestamped `now`
   are throttled to one per gauge per 305 s to stay inside Rainlog's limit;
   intermediate ones are dropped rather than buffered. Readings carrying a real
   timestamp are never throttled, and the WU relay is never throttled.

The bridge also NATs for its clients, so the console gets real internet for NTP
and vendor services. Some consoles refuse to upload until their clock syncs.

Readings mostly survive a flaky uplink: the forwarder keeps a store-and-forward
retry buffer mirrored to flash, so a reboot or an update does not drop an
undelivered reading. It drains newest-first, because the WU protocol's rain
fields are cumulative and the freshest reading already carries the full total.

The buffer is bounded rather than unlimited, and discards on purpose: 24
entries (oldest evicted first), 30 delivery attempts, and a 6 hour age cap. The
flash mirror is also throttled to one write per 15 s, so a crash inside that
window loses whatever changed since the last write.

## Hardware

**Waveshare ESP32-C6-LCD-1.47.** RISC-V single core, WiFi 6, 1.47 inch 172x320
ST7789 LCD.

These units are **ESP32-C6FH8 with 8MB flash**, despite the Waveshare spec and
demo claiming 4MB. Check yours with `esptool flash-id`.

**LILYGO T3 LoRa32 V1.6.1 (433 MHz SX1278).** Our board identifies as an
ESP32-PICO-D4 with 4MB flash. Its 128x64 SSD1306 OLED uses I2C address `0x3C`,
SDA GPIO21 and SCL GPIO22. It has a single-color status LED on GPIO25 and a
BOOT button on GPIO0. The SX1278 supports receive-only La Crosse TX5U and AcuRite Iris OOK
reception. See [weather radio reception](main/radio/README.md) for build
options and console controls.

Flash size fixes the partition layout and the layouts are not interchangeable,
so there is one image stream per variant, keyed on `BOARD_ID` (`main/board.h`).

| Variant | `BOARD_ID` | Partitions | Status |
|---|---|---|---|
| C6FH8 (8MB) | `esp32-c6fh8-lcd-1.47` | `partitions.csv` | Built and shipping |
| LILYGO T3 V1.6.1 (4MB) | `lilygo-t3-v1.6.1-sx1278` | `partitions-lilygo.csv` | OLED and compact UI supported |
| C6FH4 (4MB) | `esp32-c6fh4-lcd-1.47` | `partitions-c6fh4.csv` | Staged, untested (no hardware on hand) |

The C6FH4 layout has no `storage` partition, so the retry buffer and statistics
would not persist across reboots on that variant. The LILYGO layout reserves
128KB for LittleFS alongside two 1.875MB OTA app slots.

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
git submodule update --init --recursive  # pinned SX127x radio driver
./build.sh                               # build the C6
BOARD=lilygo ./build.sh                  # build the LILYGO
PORT=/dev/ttyACM3 ./build.sh flash        # C6: build and flash
BOARD=lilygo PORT=/dev/ttyACM2 ./build.sh flash  # LILYGO: build and flash
```

`config.h` is gitignored and holds only compile-time fallbacks; everything
user-facing is provisioned at runtime. Never commit WiFi passwords or PWS keys.

Flashing requires an explicit `PORT`; prefer `/dev/serial/by-id/...` over a
numbered device node. Interactive `flash` also starts the serial monitor.
C6 builds use `build/` and `sdkconfig`; LILYGO builds use `build-lilygo/` and
`sdkconfig.lilygo`, so changing boards does not overwrite the other build.
`./build.sh flash` writes the bootloader, partition table, OTA data, and app.

`managed_components/` is committed rather than fetched. `dependencies.lock`
(C6) and `dependencies.esp32.lock` (LILYGO) pin exact versions. Once the Docker
image is available, firmware compilation can run offline.

### Manual firmware installation

The **Firmware** tab's **Manual update** section accepts the app `.bin` from
`build/rainlog-wireless-bridge.bin` (C6) or
`build-lilygo/rainlog-wireless-bridge.bin` (LILYGO). Debug builds use their
`build-debug/` or `build-lilygo-debug/` directory. Build with the current project
so the file includes its board identity; merged images and older untagged apps
are rejected. Choose a trusted image, review its version and board, then click
**Upload & install**. Keep power connected until the bridge reboots.

Only the inactive app slot is written. Identity, capacity and complete image
integrity are checked before selecting it for boot. A failed or interrupted
upload preserves the current firmware and settings. The bootloader rolls back
if the new app resets before completing its startup health check. Manual
installation also permits the same version or a downgrade.

### Configuration web UI

The setup page is a TypeScript + esbuild subproject in `web/`, built into
self-contained pages under `main/web/`. The firmware embeds the radio-enabled
or radio-disabled gzip page selected by its compiler flags. The Node build
minifies the page and compresses it with 100 Zopfli iterations.

```sh
cd web && npm install && npm run build
```

**The generated pages under `main/web/` must be committed**: the IDF image has no
Node, so the firmware build never runs npm. CI fails if it is stale.

### Optional JavaScript debug console

Normal builds contain serial logs only. Enable the MeshCore-style Duktape
console with `DEBUG_CONSOLE=1`; debug builds use separate output/configuration
files (`build-lilygo-debug/`, `sdkconfig.lilygo.debug`, or the C6 equivalents).

```sh
BOARD=lilygo DEBUG_CONSOLE=1 ./build.sh
BOARD=lilygo DEBUG_CONSOLE=1 PORT=/dev/serial/by-id/<device> ./build.sh app-flash
BOARD=lilygo DEBUG_CONSOLE=1 PORT=/dev/serial/by-id/<device> ./build.sh monitor
```

Send newline-terminated commands at 115200 baud over the board's existing USB
connection. The LILYGO uses UART0; the C6 uses native USB serial/JTAG.

```text
js 1 + 2
js JSON.stringify(hw.heap())
js JSON.stringify(hw.wifi())
js JSON.stringify(hw.stats())
js hw.millis()
js hw.gpio(0)
js print("hello")
js off
```

Variables persist between commands. `js off` frees the engine, which starts
again on the next expression. Commands are limited to 2048 bytes and 200 ms of
metered execution, matching MeshCore. Built-in objects and strings are stored
in flash. Engine allocations are capped at 48 KiB of internal RAM; errors
release the engine.

The enabled console provides local administrative access:

```text
js settings.get()
js settings.set({display_dim_pct:10, display_dim_after_s:60})
js web.scanLive()
js web.clients()
js web.rename("AA:BB:CC:DD:EE:FF", "Weather station")
js web.otaStatus()
js web.reboot()
```

`settings.get()` includes Wi-Fi passwords and WU keys. Partial updates persist
Wi-Fi settings, WU mappings, host/path overrides, brightness/dimming and LED
level. Display/LED tuning applies immediately; reboot to apply network changes.
The console also exposes Wi-Fi testing, browser-style saves, OTA check/install,
factory reset, stats clearing and log levels. Hardware pins and fonts remain
compile-time choices reported by `hw.buildInfo()`. See
[console reference](components/debug_console/README.md) for all calls, settings,
limits and source provenance.

### Display backends

Both panels use ESP-IDF's built-in `esp_lcd` drivers. `main/ui/display_st7789.c`
retains the Waveshare panel's RAM byte order, voltage/gamma settings, 34-column
offset, mirroring and inversion. `main/ui/display_ssd1306.c` transmits the
native 1-bit SSD1306 page buffer directly; non-black colors light the OLED.
The OLED renderer uses a single 1 KiB buffer, saving 16 KiB by removing the
RGB565 framebuffer and separate conversion buffer.
Font selection is a compile-time `BOARD_DISPLAY_FONT` define in each board
branch of `main/board.h`: `BOARD_FONT_6X10` for LILYGO (18 columns, six rows)
and `BOARD_FONT_8X13` for C6. Both regular and bold faces are generated by
`tools/gen-font.py`; the 6x10 bold face uses horizontal pixel expansion.
The OLED layout and credential paging use the selected font metrics; 6x10
setup credentials cycle through 18-character chunks. A 14-pixel left strip shows
a 90-degree Rainlog wordmark in Arial Narrow Bold and a manually corrected,
symmetric 1-bit icon traced from the original logo. The font uses no antialiasing,
and the solid underline leaves clearance around the g. Generate the selected
bitmap with `tools/gen-oled-logo.py` (preview: `tools/oled_logo.png`).
The same script writes `tools/oled_logo_preview.png`, showing the final
header and layout at actual size and enlarged. It requires Pillow and a local
Arial Narrow Bold font; the corrected 1-bit icon is embedded in the script.
The traced vector source is `tools/icon-512-traced.svg`. The final horizontal
artwork wraps right by two pixels before rotation.
Brightness controls LCD backlight PWM or
OLED contrast. Flush finishes before the single drawing buffer is reused.

Run `./tests/test-display.sh` for host checks of RGB565 colors, drawing bounds,
alpha handling, brightness and OLED page packing on both screen geometries.
Controller datasheets and their source links are in `datasheets/`. Both display
backends were flashed and checked on hardware on 2026-10-07.

### Reference drivers

`./fetch-demos.sh` downloads both official reference projects locally (gitignored):

- `ESP32-C6-LCD-1.47-Demo/`: Waveshare examples for ST7789, WS2812, SPI, and SD init sequences.
- `LilyGo-LoRa-Series/`: LILYGO examples and prebuilt firmware for the T3 LoRa32 V1.6.1, including the 433 MHz SX1278 variant. Select `T3_V1_6_SX1278` in an example's `utilities.h`. Start with `examples/ArduinoLoRa/LoRaReceiver/` or `LoRaSender/`; prebuilt 433 MHz V1.6.1 samples are under `firmware/`.

Existing demos are skipped. Use `./fetch-demos.sh --force` to re-download and overwrite both. The LILYGO download follows upstream `master`; it is reference code, not a pinned firmware build dependency.

## Firmware updates (OTA)

The bridge checks 2 seconds after its uplink comes up, then backs off by
doubling after each check until it settles at a randomized 12 to 24 hour
interval (roughly a day of uptime to get there). Updates stream into the inactive slot, are verified against the
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
not match. A manifest with no `board` field at all is accepted, for
compatibility with pre-board releases.

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
  signs everyone out. HTTP Basic auth with the same password works for
  scripting, and no `WWW-Authenticate` is ever sent, so browsers never pop a
  native dialog.
- **Only the sign-in form throttles failed attempts** (1 s penalty). The Basic
  auth path has no throttle, so password guessing against it from the LAN runs
  at full speed. Since a correct guess returns the WU upload keys, treat the
  bridge WiFi password as the real boundary and make it a good one.
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
