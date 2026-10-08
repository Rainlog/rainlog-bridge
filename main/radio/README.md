# Weather radio reception

The LILYGO T3 LoRa32 V1.6.1 uses its SX1278 in continuous OOK receive mode.
SPI pins are SCLK 5, MISO 19, MOSI 27, CS 18 and reset 23. DIO2 (GPIO32)
feeds ESP-IDF RMT pulse capture. The upstream C driver is pinned in
`components/sx127x`, an Apache-2.0 Git submodule.

Reception defaults to 433.920 MHz, 250 kHz bandwidth and OOK peak floor 15.
Sub-80 microsecond glitches are merged in software. Validated packets are
logged and retained in a 16-entry RAM history. A separate inventory retains
up to 16 distinct sensors per boot, with packet counts and the latest known
rainfall counter, so repeated packets do not displace quieter devices.
The Devices tab shows this inventory. Saved mappings associate sensors with
Rainlog gauges. Mapped rainfall is encoded and forwarded through the existing
retry worker. Repeats remain visible
in the raw packet history.
RSSI is sampled after capture, so it can reflect background noise rather
than the transmitting station's signal strength. The Devices cards show this
last-received estimate as four relative bars and dBm, with the sampling caveat
in the tooltip. Saved sensors not yet seen this boot have no signal indicator.

Supported protocols:

- La Crosse TX5U: 44-bit OOK PWM, checksum, even parity and repeated counter
  fields. The 12-bit tipping counter uses 0.2667 mm per tip.
- AcuRite Iris 5-in-1: 64-bit OOK PWM, additive checksum and parity. Message
  0x31 contains wind and a 14-bit rainfall counter (0.254 mm per increment).
  Message 0x38 contains wind, temperature and humidity.

Protocol details and TX5U sample provenance are recorded in the sibling
`home-assistant-edit/RTL-SDR.md` and
`home-assistant-edit/rtl-sdr/samples/lacrosse-tx5u/README.md`, with the upstream
rtl_433 `acurite.c` as the AcuRite format reference. The local Ambient
WH65B/WS69 array transmits near 915 MHz using FSK. This 433 MHz SX1278 cannot
receive it; that requires different radio hardware.

`BOARD=lilygo ./build.sh` enables reception. `RADIO=0` disables it at compile
time; C6 builds default to disabled. There is no transmit API.

The board build supplies `RAINLOG_RADIO_MHZ` (433 for this LILYGO, 0 for
boards without a radio). `weather_protocols.h` derives per-device build
guards: TX5U and Iris require 433 MHz; the future Ambient array decoder
requires 915 MHz. Decoder functions, declarations and pulse state for
unsupported bands are omitted at compile time. `RADIO=0` disables all
protocol guards. The Ambient guard reserves its band; its FSK decoder and
a compatible 915 MHz board driver are not implemented yet.
With `DEBUG_CONSOLE=1`, the serial console exposes:

```text
js radio.status()
js radio.packets()
js radio.receive(false)
js radio.receive(true)
js radio.tune(433930000, 250000, 15)
```

Console tuning and receive controls last until reboot. The website radio
checkbox, Wi-Fi interception checkbox and sensor-to-Rainlog mappings persist in NVS and apply on reboot.
The radio-enabled Devices tab lists recently received sensors and supports
selection from detected sensors, Rainlog station IDs and PWS keys. Mappings can also be
managed through `settings.get().radio_map` and `settings.set({radio_map: [...]})`.
The frequency argument
is Hz (400 to 470 MHz), bandwidth is Hz and peak floor is an integer byte.

Run `tests/test-weather-decode.sh` for recorded TX5U pulse fixtures, generated
AcuRite frames and single-bit corruption checks. Regenerate fixtures with
`tools/gen-radio-fixtures.py` while the original sibling IQ captures are
available.

## Validation on the local board

On 2026-10-07 the SX1278 returned version 0x12. Receive, pause/resume, tuning
and console validation passed on ttyACM2. Temporary raw pulse tracing also
captured the neighboring AcuRite 606TX (ID 229): frame `e580f9fa`, with a
matching LFSR checksum, reporting 24.9 C. That validates the RF/pulse path;
606TX temperature packets are not part of the rain decoder. No live Iris or
TX5U packet passed decoding during the initial test. TX5U decoding passes recorded
captures, and Iris decoding passes generated pulse and corruption tests.
The temporary trace was removed from the final firmware.

After the TX5U was powered on later that day, ID 7 was decoded live with
counter 3, including the repeated packet pair. It appeared in the website's
received sensor list. Battery replacement can change sensor IDs, so check the
list and update the mapping after replacing a sensor battery.

Disabling Wi-Fi interception turns off the bridge SoftAP, DNS interception
and WU capture endpoints, including their HTTPS server. Home Wi-Fi and the
setup page remain available on the home LAN. The setup password remains the
Bridge Wi-Fi password, even when that Wi-Fi network is disabled. Home Wi-Fi
must be configured before disabling interception. Both switches apply after
Save and reboot. Radio builds default to interception off; unconfigured boards
keep setup Wi-Fi available until provisioning. Saved choices override defaults.

## Radio uploads

Sensors with a Rainlog or device-keyed WU uploader and a decoded rain counter upload. The forwarder task
polls the latest sensor inventory each second and queues encoded snapshots,
so TLS never runs in the pulse capture task or on the encoder call stack. Each gauge queues a snapshot every 300 seconds, including unchanged totals
and cached readings when no new frame has arrived. SNTP must establish a wall
clock first. `dateutc` is the snapshot time, preserved through the existing
LittleFS retry queue.

`ID=Rainlog<gauge_id>` and `PASSWORD=<PWS key>` come from the saved Rainlog mapping. WU-only sensors omit these credentials and skip Rainlog entirely; WU credentials are selected by model/ID/channel. Combined uploaders share one encoded snapshot. Legacy WU gauge mappings remain readable and keep working.
`totalrainin` is the cumulative counter in inches, using 0.0105 in per TX5U tip
and 0.01 in per Iris tip. Counter baselines and expanded totals are stored in
the `radio_rain` NVS namespace (legacy Rainlog gauge keys, or sensor identity keys for WU-only sensors) before transmission and only rewritten when
they change. Normal rollover is expanded; other decreases and sensor identity
changes rebase without adding old rain. A same-ID reset near the end of its
counter range cannot be distinguished from rollover, and multiple complete
wraps during a reception outage cannot be recovered. Iris snapshots include
wind and the temperature/humidity fields actually available in that frame.
Hourly and daily rain are omitted rather than guessed.

Rainlog uploads carry individual `bridge_model` (compile-time board ID),
`bridge_id` (factory STA MAC, 12 lowercase hex digits), `sensor_model`,
`sensor_id` and `sensor_channel` parameters. TX5U has an empty channel. The
existing `rlbridge` firmware-version marker remains, and `rlsource=radio`
distinguishes generated readings. Bridge identity is added to Wi-Fi forwards
as well. Private identity fields are stripped from optional WU relays.
These fields are diagnostic metadata; server-side deduplication is unchanged.

Each Rainlog gauge may be mapped to only one radio sensor. Mapping a gauge
already seen on a Wi-Fi device this boot is rejected by configuration validation
(including console settings) and by the browser. Radio mappings reserve their
gauge even while reception is paused. Wi-Fi capture returns `RADIOASSIGNED`
for those gauges, and conflicting older Wi-Fi retry entries are discarded.
The Wi-Fi inventory is not persistent, so after reboot an existing radio
reservation still prevents a returning Wi-Fi station from uploading.

Run `tests/test-rain-upload.sh` for encoding, URL escaping, source identity,
rollover/reset/replacement, cadence, persistence failures and recovery checks.
