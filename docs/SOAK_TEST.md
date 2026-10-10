# Soak test acceptance

The port is intended for unattended, long-duration ESP8266 operation. Feature acceptance is not complete until the stability tests below pass on the target board.

## Stage 1 - 7 day baseline

Run continuously for at least 7 days with:

- Wi-Fi AP loss/restoration every 15 minutes
- MQTT broker loss/restoration every 10 minutes
- continuous HTTP polling from LiveReadings
- relay switching every 30 seconds through alternating Web, MQTT and Modbus commands
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

## Stage 2 - fault injection

During a separate 24 hour run:

1. Remove Wi-Fi for 30 minutes and restore it.
2. Restart the MQTT broker repeatedly during active relay traffic.
3. Open and close the LiveReadings, Files and Backup pages repeatedly from several browser sessions, while respecting the configured HTTP socket cap.
4. Disconnect a DS18B20 probe and reconnect it. Sensor failure must not block other tasks.
5. Exercise Modbus clients that connect, stop transmitting, and disconnect unexpectedly. Receive timeouts must recover the server.
6. Poll an external Modbus slave from two automation sensor slots; verify signed/float decoding, word order, timeout backoff, and rule evaluation while the slave is unplugged and reconnected.
   Configure one rule to write a coil on another board. Verify the matched
   and cleared states, unit ID and zero-based address; disconnect the target
   for several minutes and confirm a queued write is retried after recovery.
7. Attempt malformed file-manager, restore, timer and relay requests. Invalid input must not change persisted configuration.
8. Upload a deliberately invalid OTA image. The currently running image must remain the selected boot image.
9. Complete one valid OTA update, reboot, and confirm configuration/LittleFS survival.
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
