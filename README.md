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

The firmware also carries minimal compiled recovery copies of the core web pages.
At boot they are written only when the corresponding LittleFS file is missing. This
means a firmware-only flash still provides a recovery UI; uploading the filesystem
image replaces those recovery pages with the files in `data/`.

PlatformIO filesystem tasks are available through the ESP8266 platform:

```bash
pio run -t buildfs
pio run -t uploadfs
```

The filesystem partition is `webfs` in `partitions.csv` and occupies
`0x290000..0x3FFFFF` on the 4 MB flash device.

## Implemented modules

- AP + station Wi-Fi with reconnect/backoff
- MQTT relay control and state publishing
- Home Assistant MQTT discovery
- relay command queue / single GPIO owner
- schedule timers
- Modbus/TCP relay coils
- DS18B20 acquisition task
- ACS712 acquisition task
- NTP/time service
- LittleFS web filesystem
- configuration persistence in NVS
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

The layout is sized for a 4 MB HUZZAH flash device.

## Build validation

The project is compiled with PlatformIO, ESP8266_RTOS_SDK 3.4, and Espressif's
GCC 8.4.0 toolchain. A clean `pio run` is used as the release validation step.
