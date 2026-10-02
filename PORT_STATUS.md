# SmartConfig-AI ESP8266 RTOS port status

Target: ESP8266_RTOS_SDK release/v3.4. Source behavior: Kbdeir/ESPRelayNode8266.

## Implemented runtime foundation

- NVS fixed-size configuration
- AP+STA Wi-Fi with retry backoff
- native HTTP server with bounded sockets
- serialized LittleFS access and streaming static files
- relay task as sole GPIO writer
- esp-mqtt publish queue and SmartConfig topic semantics
- heap/reconnect health telemetry
- NTP/SNTP time service

## Added in this revision

| Module | Status | Notes |
|---|---|---|
| Schedule timers | Implemented baseline | 4 persisted timers; once/daily/weekly/monthly, relay ON + duration OFF |
| Modbus TCP | Implemented baseline | coils 0..N-1, FC01/05/15, one bounded client at a time |
| Home Assistant discovery | Implemented | retained switch, DS18B20 and ACS discovery, staggered publication |
| Backup / restore | Implemented | configuration, credentials, relay topics and timers |
| File manager | Implemented | LittleFS list/download/upload/delete/rename backend; streaming I/O |
| Crash/core-dump UI | Implemented as ESP8266 crash diagnostics | persistent abnormal reset class and boot count; no fabricated ESP32 backtrace |
| DS18B20 | Implemented baseline | non-blocking conversion wait, two configured GPIOs, cached readings |
| ACS712 | Implemented baseline | ADC sampling task, 5/20/30 A sensitivity, MQTT publish |
| OTA | Implemented | dual OTA slots, streamed upload, image validation, boot-slot switch, reboot |

## Deliberately deferred

AutomationRules, remote sensors, input/relay mapping, WireGuard, HomeKit/Alexa, ESP-NOW, OLED, ADS1X15/HST, CT/EmonLib, water/pressure/sonar sensors.

## Stability constraints

- Network callbacks enqueue or update state only.
- MQTT command echoes cannot create relay publish loops.
- LittleFS access is mutex serialized and uploads use one streaming writer.
- HTTP static/download/upload paths use fixed 768/1024-byte buffers.
- Sensor reads are done in a dedicated task; HTTP returns cached sensor values.
- OTA writes directly to the inactive slot and never buffers the image in RAM.
- Flash configuration writes occur only on explicit saves/restores.
- Modbus accepts one client at a time with receive timeout.

## Hardware validation still required

This environment does not include the ESP8266 GCC toolchain/SDK, therefore firmware compilation and on-board soak testing must still be run on the target development environment. DS18B20 timing and ACS712 calibration in particular must be verified on the actual SmartConfig hardware revision.
