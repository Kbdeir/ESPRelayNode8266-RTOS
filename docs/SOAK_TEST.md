# Soak test acceptance

The port is intended for unattended, long-duration ESP8266 operation. Feature acceptance is not complete until the stability tests below pass on the target board.

## Stage 1 - 7 day baseline

Run continuously for at least 7 days with:

- Wi-Fi AP loss/restoration every 15 minutes
- MQTT broker loss/restoration every 10 minutes
- continuous HTTP polling from LiveReadings
- relay switching every 30 seconds through alternating Web, MQTT and Modbus commands
- Rapidly alternate On/Off on Live Readings with MQTT connected, then stop.
  Verify switching stops, no state payload is sent to a relay `/set` topic,
  and TTL expiry does not re-arm TTA through a self-delivered command.
- DS18B20 acquisition enabled at the configured cadence
- ACS712 acquisition and MQTT publication enabled where fitted
- Timer schedules crossing midnight and weekday/month boundaries
- Home Assistant discovery republished after MQTT reconnects
- File Manager list/download activity every 5 minutes
- a configuration backup once per hour
- one small LittleFS upload/delete cycle every hour

Acceptance:

- zero watchdog resets
- zero unexpected reboots
- no progressive decline in free heap after the first hour
- queue use remains bounded
- HTTP remains responsive while MQTT is offline
- local relay and Modbus control remain functional while MQTT is offline
- configuration survives repeated Wi-Fi and MQTT reconnections
- Modbus remote sensor values recover after slave loss and become stale after 300 seconds without a valid response
- largest free heap block recovers after Modbus poll connections close
- outbound Modbus coil writes recover after destination loss without blocking rule evaluation
- filesystem stays mountable and files remain intact
- timer state remains correct after network outages
- with newlib-nano, fractional temperature/current/Modbus values remain correct
  in MQTT payloads, web JSON, and configuration backup/restore

## Stage 2 - fault injection

During a separate 24 hour run:

1. Remove Wi-Fi for 30 minutes and restore it.
2. Restart the MQTT broker repeatedly during active relay traffic.
3. Open and close the LiveReadings, Files and Backup pages repeatedly from several browser sessions, while respecting the configured HTTP socket cap.
4. Boot with DS18B20 and ACS disabled, enable DS18B20 through Config, and save
   without rebooting. Confirm fractional readings on LiveReadings and
   `SensorStatus.json`, and on MQTT if already connected. Verify both probes,
   then single-probe operation with both GPIO fields set to the same pin.
   Disconnect a DS18B20 probe and reconnect it. The missing reading must become
   unavailable and recover without a reboot; invalid/CRC-failed data must not
   trigger temperature relay control. Sensor failure must not block other tasks.
   Disable DS18B20, verify readings become unavailable, then re-enable it and
   verify recovery without allocating a sensor task or mutex. Confirm GPIOs in use
   by relays/digital inputs and UART/flash/status-LED GPIOs cannot be selected.
   Leave AdvancedTemperatureConf open while warming one probe; both displayed
   readings must refresh without reloading or overwriting an unsaved threshold
   edit. Verify missing readings show unavailable rather than zero or a forced
   -127. Boot with an older saved digital-input mapping on a DS18B20 GPIO and
   confirm the input is suppressed while sensor acquisition continues.
5. Exercise Modbus clients that connect, stop transmitting, and disconnect unexpectedly. Receive timeouts must recover the server.
6. Poll an external Modbus slave from two automation sensor slots; verify signed/float decoding, word order, timeout backoff, and rule evaluation while the slave is unplugged and reconnected.
   Configure one rule to write a coil on another board. Verify the matched
   and cleared states, unit ID and zero-based address; disconnect the target
   for several minutes and confirm a queued write is retried after recovery.
7. Attempt malformed file-manager, restore, timer and relay requests. Invalid input must not change persisted configuration.
   Repeatedly save Config, Relay Config, Inputs, Temperature, Timers and
   Automation while MQTT/HA discovery and browser polling are active. Continue
   through multiple LittleFS metadata compactions; no HTTP stack-canary panic
   or watchdog reset is acceptable. Exercise backup restore and reset too.
   Run `python tools/test_http_stack.py` after each firmware build, then check
   `/api/heap` for at least 512 bytes of `httpStackFree` after these operations.
   Upload multiple HTML pages through Files, including `config.html`.
   When gzip compression is available, confirm `/config.html.gz` replaces
   `/config.html` and the Config link still loads with `Content-Encoding: gzip`.
   Reboot and verify the compressed upload is still served; upload the
   uncompressed page alone and confirm it takes precedence without a gzip header.
   Verify Timer1 remains uncompressed and its template values still render.
8. Upload a deliberately invalid OTA image. The currently running image must remain the selected boot image.
9. Complete one valid OTA update, reboot, and confirm configuration/LittleFS survival.
   Poll `/api/heap` after exercising every HTTP handler. Confirm `httpStackFree`
   retains at least 512 bytes of headroom, and free heap/largest block stabilize
   under continuous temperature polling. Repeat HA discovery with MQTT slowed
   or disconnected; telemetry must not stall the scheduler, and the large MQTT
   slot must be released on publish/queue failure.
   With a slow broker, each discovery message must queue once per batch while
   the worker waits for its current slot. A ten-second queue timeout must abort
   the batch and release the worker, not replay earlier messages indefinitely.
   Disconnect/reconnect during discovery and confirm a fresh batch is requested.
10. Trigger an abnormal reset in a controlled test build and verify that the crash-diagnostics page reports the reset class after reboot.

## Stage 3 - 30 day qualification

After the 7-day and fault-injection tests pass, run the same normal workload for 30 days.

Record at least once per hour:

- uptime
- free heap
- minimum observed free heap
- Wi-Fi reconnect count
- MQTT reconnect count
- relay command count
- HTTP availability
- DS18B20/ACS712 error state
- filesystem usage

A slow monotonic heap reduction, increasing reconnect storm, filesystem corruption, watchdog event, or unexplained reboot is a release blocker.
