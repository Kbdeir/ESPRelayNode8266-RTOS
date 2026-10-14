# ESPRelayNode8266 — ESP8266 RTOS SDK / PlatformIO port

Native ESP8266_RTOS_SDK 3.4 port of the SmartConfig-AI / ESPRelayNode8266 firmware.
The target is the same Adafruit HUZZAH ESP8266 board used by the original project.

## PlatformIO quick start

Open this folder directly in VS Code with PlatformIO installed.

On Windows, install the ESP8266 RTOS SDK 3.4 compiler package once before the first
build. The script downloads Espressif's official GCC 8.4 toolchain and registers it
with PlatformIO:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\bootstrap_toolchain.ps1
```

Then build, upload, and monitor with:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

Default environment:

```ini
[env:huzzah_rtos]
board = huzzah
framework = esp8266-rtos-sdk
```

The PlatformIO platform/framework are pinned in `platformio.ini` for reproducible builds.
On the first build, `tools/pio_prepare.py` automatically fetches the pinned LittleFS
source component into `components/` if it is not already present. MQTT is supplied by
the pinned ESP8266 RTOS SDK.
Git and Internet access are therefore required only for that first dependency fetch.

## DS18B20 temperature readings

Enable DS18B20 and save the configuration. Sampling starts without a restart,
using deadline-based polling in the existing schedule task. The default tank GPIO
is 14 and the panel GPIO is 12; neither can also be an active relay or digital
input. Use one probe per GPIO, or set both GPIO fields to the same pin for a
single tank probe. Sensor readings appear in LiveReadings and `SensorStatus.json`
after the 760 ms conversion wait and the next sampling cycle.
The temperature configuration page refreshes readings every 2.5 seconds without
overwriting unsaved form edits. Missing readings remain unavailable; no synthetic
zero or -127 temperature is substituted. Existing saved digital-input mappings
on a sensor GPIO are suppressed at runtime and cannot silently disable sensing.

Use three-wire power: VDD to 3.3 V, GND to board ground, and DQ to its configured
GPIO with an external 4.7 kOhm pull-up to 3.3 V. Parasite-power operation and
multiple probes sharing one GPIO are not implemented. Scratchpad CRC failures
or disconnected probes appear as unavailable and do not trigger temperature
relay control. GPIO numbers are the ESP8266 GPIO numbers, not board pin labels.

Host regression checks: `python tools/test_ds18b20.py` (requires `cc`, or WSL
with `cc` on Windows). The test mocks the probes/RTOS; final acceptance requires
real probes on the board. Page polling checks: `node tools/test_temperature_page.js`.

## Web UI / LittleFS

The PlatformIO filesystem source directory is the top-level `data/` directory.
It contains the web pages used by the port:

- `data/config.html`
- `data/LiveReadings.html`
- `data/RelayConfig.html`
- `data/Timer1.html`
- `data/Files.html`
- `data/Backup.html`
- `data/coredump.html`
- `data/FirmwareMaintenance.html`
- `data/index.html`

`platformio.ini` explicitly selects LittleFS:

```ini
board_build.filesystem = littlefs
```

The firmware also carries compiled recovery copies of the core web pages.
At boot, it seeds missing pages and replaces pages that differ from those
copies. A firmware-only serial flash therefore still provides the web UI.
If a page exists only as `.html.gz`, startup preserves that compressed
upload. Static page requests use the uncompressed file when present, or
serve its `.gz` counterpart with `Content-Encoding: gzip` otherwise.
Build a 2 MiB LittleFS image with `pio run -e huzzah_rtos -t buildfs`.
The project supplies the filesystem geometry from `partitions.csv`, since
the ESP8266 RTOS linker script does not expose PlatformIO's Arduino-style
filesystem symbols. To upload the web pages over serial, close the monitor
and run `pio run -e huzzah_rtos -t uploadfs --upload-port COM3` (substitute
your board's port). The project-owned upload action reads and saves a full
4 MiB flash backup under `.pio/flash-backups/`, confirms the partition table,
and merges `data/` into the existing LittleFS image. It verifies that files
outside `data/`, including `.smartcfg`, remain unchanged before writing only
the `webfs` partition. If backup, mount, or repack fails, it does not flash.

The filesystem partition is `webfs` in `partitions.csv` and occupies
`0x200000..0x3FFFFF` (2 MiB) on the 4 MB flash device.

## Implemented modules

- AP + station Wi-Fi with reconnect/backoff
- MQTT relay control and state publishing
- Home Assistant MQTT discovery
- relay command queue / single GPIO owner
- schedule timers
- Modbus/TCP relay coils
- automation rules with local, MQTT, or Modbus TCP sensor conditions,
  and local relay or outbound Modbus TCP coil actions
- DS18B20 and ACS712 acquisition without a separate sensor task
- NTP/time service
- LittleFS web filesystem
- configuration persistence in LittleFS with migration from older NVS data
- web configuration/status endpoints
- file manager
- backup / restore
- persistent reset/crash diagnostics UI
- dual-partition OTA
- health/heap/reconnect counters

## Stability design

This port is intentionally structured around bounded FreeRTOS tasks and queues rather
than reproducing the original Arduino `loop()` architecture. HTTP handlers return
cached runtime snapshots and do not synchronously read sensors or perform network
reconnect work. Relay GPIO is owned by one task. MQTT callbacks enqueue work. LittleFS
writes are serialized. See `docs/ARCHITECTURE.md` and `docs/SOAK_TEST.md`.

## Flash layout

See `partitions.csv`:

- NVS
- OTA metadata
- PHY init
- OTA slot 0
- OTA slot 1
- LittleFS `webfs`

The layout is sized for a 4 MB HUZZAH flash device. Each OTA app slot is
0xF0000 bytes (960 KiB). ESP8266 maps only 1 MiB of flash during early app
startup, so both slots must end before their mapped window boundary. The
firmware uses size optimization and PlatformIO enforces the 960 KiB limit.
LittleFS occupies the last 2 MiB at 0x200000.

**Do not deploy a partition-layout change over OTA.** The 1-to-2 MiB LittleFS
expansion moves its start from 0x300000 to 0x200000. Flash the partition table,
firmware, and a matching 2 MiB LittleFS image together over serial. A firmware-
only upload cannot migrate the old filesystem. A fresh filesystem replaces all
files stored there, including `.smartcfg`; NVS is not erased.
Remote Modbus sensors are configured in Automation > Remote Sensors using a
zero-based address, function code 01/02/03/04, value type, scale, and poll rate.
For an outbound rule action, select Modbus TCP coil as the rule output and
enter the destination IPv4, TCP port, unit ID, and zero-based coil address.
The rule sends function code 05 (Write Single Coil). By default it writes
the opposite state when the condition clears; this can be disabled per rule.
Failed writes are retried every five seconds while the rule state remains
current. The destination must accept function code 05 and be reachable over
the board's Wi-Fi network.

## Build validation

The project is compiled with PlatformIO, ESP8266_RTOS_SDK 3.4, and Espressif's
GCC 8.4.0 toolchain. A clean `pio run` is used as the release validation step.

`CONFIG_NEWLIB_NANO_FORMAT=y` selects newlib-nano. The application explicitly
links `_printf_float` and `_scanf_float` for cJSON and automation numeric
serialization. Hot sensor/configuration telemetry instead uses integer-based
fixed-point formatting to avoid floating formatter allocations. Nano formatting
does not support 64-bit integer conversions or
positional arguments. Runtime heap savings must be measured on the board.

## Heap validation

Sensor acquisition shares the existing schedule task and waits for conversions
by deadline, without allocating a sensor task or snapshot mutex. Temperature JSON
streams through a 128-byte buffer without a cJSON tree. Static pages use one open
LittleFS stream and a 256-byte transfer buffer, including gzip uploads.

MQTT keeps three 32-byte telemetry slots and one 672-byte discovery slot instead
of four large slots. QoS 0 telemetry never waits for a slot or queue space; a
congested sample is dropped and the next sampling cycle retries. Discovery keeps
its full payload capacity and retries the current message for up to ten seconds
when the pool is busy, without replaying previously queued messages. A timeout
aborts the batch and frees the worker; reconnecting MQTT or explicitly requesting
discovery starts a new batch. Timing state is already published by MQTT connection
sync and timing changes, so discovery does not enqueue duplicate timing samples.
Discovery buffers live only
on the temporary worker stack, with extra stack headroom for those buffers.

The heap-optimized build uses 18,408 bytes of static RAM versus 21,532 before
these changes. It also removes the 2,304-byte sensor stack. HTTP now reserves
9,472 bytes (1,280 more than the original 8,192): the 7,168-byte trial overflowed
during Config save when LittleFS compacted metadata and wrote flash. Task/mutex overhead and formatter allocations
are additional savings; the largest free block is not guaranteed to grow by
the same amount. Compare `/api/heap` after reboot under the same MQTT/Wi-Fi
workload and number of browser sessions. Its `httpStackFree` is the HTTP task's
lowest remaining stack space, in bytes, since startup. Exercise saves, reset,
backup/restore, timers, files and OTA, including repeated saves that force
LittleFS metadata compaction. Require at least 512 bytes of remaining stack.

Regression checks: `python tools/test_ds18b20.py`,
`python tools/test_mqtt_pool.py`, `python tools/test_web_gzip.py`,
`python tools/test_relay_mqtt.py`, `python tools/test_ha_discovery.py`,
`node tools/test_readings_page.js`, and
`node tools/test_temperature_page.js`. Relay notifications publish only to the
state/publication topics, never back to the subscribed command topic. Delayed
notifications must not become new commands or re-arm a completed TTL/TTA cycle.
GCC stack-usage reports are emitted as
`.su` files in `.pio/build/huzzah_rtos/` for call-chain review.
PlatformIO runs `tools/test_http_stack.py` whenever the firmware ELF is relinked.
Run `python tools/test_http_stack.py` separately to check all save-handler
frames against the decoded compaction call chain, with additional filesystem,
interrupt and HTTP-dispatch allowances. This regression guard is not a bound
on every filesystem state; on-device stack-watermark testing is still required.
