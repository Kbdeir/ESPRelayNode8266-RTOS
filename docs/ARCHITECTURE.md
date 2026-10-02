# Runtime architecture

The port intentionally does not reproduce Arduino's monolithic `loop()`.

- `sc_wifi`: owns Wi-Fi lifecycle and reconnect policy.
- `sc_mqtt`: owns the MQTT client and serializes outgoing publishes.
- `sc_relay`: sole relay GPIO owner. All sources submit commands through its queue.
- `sc_config`: sole persisted configuration API, backed by NVS.
- `sc_web`: native HTTP server. Handlers only read snapshots or enqueue commands.
- `sc_webfs`: web files served from LittleFS. Compiled fallback seeds the filesystem only when files are missing.
- `sc_health`: low-cost telemetry used for soak testing.

## Stability invariants

- No subsystem may call another subsystem's blocking network function from a callback.
- No configuration write from an ISR, event callback, or HTTP status handler.
- No unbounded queue waits from HTTP or MQTT callbacks.
- No periodic reboot as a normal recovery mechanism.
- Network outages must not affect local relay task scheduling.
