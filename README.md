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
- DS18B20 acquisition task
- ACS712 acquisition task
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
