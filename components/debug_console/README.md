# JavaScript serial debug console

Enable with `DEBUG_CONSOLE=1` (`RAINLOG_DEBUG_CONSOLE` in CMake). Off by default;
disabled builds contain no JavaScript engine or console input task.

The serial protocol follows MeshCore's `node_debug_js`: newline-delimited
`js <source>`, persistent globals, `js off`, and `Result=OK/ERROR` responses.
LILYGO uses UART0 at 115200 baud; C6 uses native USB serial/JTAG.
Both boards support debug builds. On the C6, a persistent VM can exhaust
Wi-Fi/TLS heap headroom under load; use `js off` after diagnostics to release it.

Duktape 2.7.0 replaces MeshCore's XS engine because these boards lack PSRAM.
Built-in objects and strings reside in flash (`DUK_USE_ROM_OBJECTS` and
`DUK_USE_ROM_STRINGS`). A writable global object inherits the ROM global;
variables persist, but built-in objects and their prototypes are immutable.
The allocator caps live payload allocations at 48 KiB (allocator headers are
additional). Execution interrupts enforce MeshCore's 200 ms budget against
runaway loops. Errors destroy the VM; the next command recreates it.
`js off` releases the VM. The console task uses a 12 KiB stack.

The console has local administrative access when compiled in. Wi-Fi passwords
and WU keys are included in `settings.get()` so they can be inspected and edited.
HTTP authentication remains unchanged; login/logout and HTML/favicon delivery
are browser transport features. Setup management actions use shared server
operations and run on the HTTP task, serialized with browser requests.
Native scan/flash/queue time is excluded from the JS execution budget.

## Settings

`settings.get()` returns a snapshot. `settings.set(patch)` validates and persists
only supplied fields, retaining all others. It returns `true`; validation or
storage failures throw. Unknown/read-only fields, embedded NULs, overlong
strings, invalid hosts/paths, duplicate gauges and out-of-range numbers fail
before any writes. Wi-Fi/WU edits mark the bridge provisioned; tuning-only
changes preserve provisioning state. An intervening browser save causes a
conflict error instead of overwriting its changes.

| Fields | Values | Applies |
| --- | --- | --- |
| `sta_ssid`, `sta_pass` | Up to 32/64 bytes; empty STA password is allowed | Reboot |
| `ap_ssid`, `ap_pass` | Nonempty SSID, up to 32 bytes; password 8-64 bytes | Reboot |
| `wu_map` | Up to 8 `{device, wu_id, wu_key}` entries; Wi-Fi MAC or `radio:model:id:channel`. Legacy `{gauge_id, wu_id, wu_key}` remains supported | Immediately |
| `rainlog_host`, `wu_host`, `ota_host` | Hostnames without scheme, port or path, up to 127 bytes | Reboot recommended |
| `wu_update_path` | Absolute path, up to 127 bytes | Reboot (capture routes) |
| `ota_manifest_path` | Absolute path, up to 191 bytes | Reboot recommended |
| `display_full_pct`, `display_dim_pct` | Integer 0-100 | Immediately |
| `display_dim_after_s` | Integer seconds, 0 disables dimming | Immediately |
| `led_level` | Integer 0-255; LILYGO is on/off, 0 disables it | Immediately |
| `provisioned` | Read-only setup state | Read-only |

WU arrays replace the entire map. A supplied empty WU key clears that key;
`web.save()` instead follows the browser form's blank-key preservation rule.
Build a large patch over several commands if it exceeds the 2048-byte line limit.
Network/host/path compile-time constants remain fallback defaults on fresh or
factory-reset boards. Brightness defaults are full 100%, dim 6%, idle 30 seconds,
LED level 24.

```text
js settings.get().display_dim_pct
js settings.set({display_dim_pct:10, display_dim_after_s:60})
js settings.set({sta_ssid:"My WiFi", sta_pass:"password"})
js settings.set({wu_map:[{device:"radio:0:7:0", wu_id:"TEST", wu_key:"key"}]})
js web.reboot()
```

## Web actions and other controls

| Console call | Equivalent action |
| --- | --- |
| `web.config()` | `/config`, browser config snapshot (Wi-Fi passwords omitted) |
| `web.save(form)` | `/save`, URL-encoded form; persists and reboots |
| `web.scan()` / `web.scanLive()` | Cached/live SSID lists |
| `web.test(ssid, pass)` / `web.testStatus()` | Temporary connection test; blank/omitted credentials use saved values |
| `web.clients()` | Client list; `you` is always false on serial |
| `web.rename(identity, name)` | Persist device name; identity is a Wi-Fi MAC or `radio:model:id:channel`; empty name clears it |
| `web.otaStatus()` / `web.otaCheck()` / `web.otaApply()` | OTA status, check and install (install reboots on success) |
| `web.reboot()` | Reboot after 1.5 seconds for response flushing |
| `web.factoryReset()` | Clear settings, stats and client names, then reboot |
| `web.clearStats()` | Clear upload statistics |

`web.call(path, arg1, arg2)` dispatches the same named management actions.
Unsupported paths throw; upload-capture routes are not exposed as management.
Wi-Fi tests and OTA calls return immediately; poll their status methods.

`hw.heap()`, `hw.millis()`, `hw.gpio(pin)`, `hw.wifi()`, and `hw.stats()` provide
read-only diagnostics. `hw.buildInfo()` reports firmware, board geometry, font
and hardware pins. Those hardware/build choices remain compile-time values.
`hw.wake()` brightens the display. `hw.logLevel(tag, level)` changes runtime log
verbosity (0 none, 1 error, 2 warn, 3 info, 4 debug, 5 verbose); console results
remain visible even when a log tag is muted. `print(...)` writes to serial.

Measured on LILYGO with management bindings: 16224 VM payload bytes at startup,
about 21 KiB peak during settings tests, about 85 KiB free heap (Wi-Fi AP running,
STA disconnected). Live TLS traffic needs its own heap headroom. Run
`tests/test-debug-console.py PORT LOG` for engine tests and
`tests/test-debug-management.py PORT LOG` for management tests; the latter
changes dim brightness, checks persistence over reboot, then restores it.
Neither integration test installs OTA or performs a factory reset.
Host checks: `tests/test-config-store.sh`, `tests/test-console-line.sh`.

`vendor/` contains generated Duktape sources and upstream license notices.
Regenerate with `tools/gen-debug-duktape.sh`; source release:
https://duktape.org/duktape-2.7.0.tar.xz . The upstream generator requires
Python 2.7 and PyYAML, so regeneration uses a container. Firmware builds use
only the generated C files and do not need Python 2.

## Memory audit

`mem` prints stored startup checkpoints, heap totals, regions, and optional
per-task allocation totals without starting JavaScript. Startup deltas are net
free-heap changes while services initialize, including concurrent task activity. `mem full` also lists allocated blocks. Build with
`BOARD=c6 DEBUG_CONSOLE=1 MEMORY_AUDIT=1 ./build.sh` for per-task attribution;
the separate audit build enables ESP-IDF heap task tracking, including deleted
tasks. Tracking adds memory overhead, so compare against a normal debug build.
Allocation ownership is the task that allocated the block: child task stacks
and driver initialization can therefore appear under `main`. Do not add the
framebuffer or task stacks again to the reported heap allocation totals.
