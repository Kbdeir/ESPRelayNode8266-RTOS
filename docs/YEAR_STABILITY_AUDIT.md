# One-year stability audit

Date: 2026-10-14. Scope: audited workspace and its installed,
pinned ESP8266 RTOS SDK 3.4, including generated GCC stack-usage reports.

## Deployment contract

- Required: 365 days unattended with zero recovery reboots.
- Loads: lights, heaters, fridges and pumps.
- Relay policy: hold the last confirmed output state during sensor/network loss;
  use OFF when no confirmed state is available. Do not confuse a queued command
  with a confirmed output. Service recovery must preserve relay GPIOs and timers.
- Power loss and hardware failure cannot be excluded by a software audit. A
  one-year guarantee is not established by a successful build or short soak.
- Verdict: NOT READY for this contract. No firmware behavior was changed by
  this audit. Automatic board reboot is not an acceptable remediation.

## Release blockers

### 1. P1: MQTT timing saves exceed the publisher stack

Evidence: `components/sc_mqtt/sc_mqtt.c:371` persists MQTT TTL/TTA updates;
line 435 creates `mqpub` with only 1,792 bytes. GCC reports `pubtask` = 256,
`sc_config_set_relay_timing` = 48 and the decoded nested LittleFS/flash save
path = 1,552 bytes, totaling 1,856 before interrupt overhead. The filesystem
path is the same compaction/rename path from the user's HTTP crash.

Trigger: changing a timing setting when LittleFS needs metadata compaction.
The HTTP-only stack guard passes and does not detect this background-task risk.

Required: budget every task/call chain, not only HTTP. Isolate persistence
from control/network tasks or size its caller with measured headroom; validate
MQTT changes through repeated compactions and allocation pressure on hardware.

### 2. P1: The fixed application queue does not bound SDK MQTT memory

Evidence: the application frees its payload slot immediately after
`esp_mqtt_client_publish` returns (`sc_mqtt.c:393`). SDK `outbox_enqueue`
allocates a node and packet buffer for each QoS message. `outbox_cleanup`
only removes CONFIRMED items, not queued/transmitted messages awaiting ACKs.
SDK `mqtt_enqueue` also discards the enqueue-allocation failure result.

Reproduced against the installed SDK with `tools/audit_mqtt_outbox.py`:
seven 672-byte packets occupy 4,704 bytes; cleanup at the nominal 4,096-byte
limit returns failure and leaves all packets allocated. Normal expiration
eventually frees them, but does not prevent a burst from exhausting heap.
This is not evidence of an endless normal-operation leak.

Required: a hard byte/item/in-flight cap before allocation, latest-state
coalescing, observable allocation failures and a bounded congestion recovery
path. Test a broker that accepts TCP/publishes but withholds acknowledgements,
not just a disconnected broker. Service recovery must not reset the board.

### 3. P1: Mount/load errors can destroy saved configuration

Evidence: `components/littlefs/sc_littlefs.c:16` formats the partition on any
mount failure, including I/O/allocation failures. Then
`components/sc_config/sc_config.c:205` rewrites the live/default/NVS candidate
on any filesystem-config load failure. Valid previous settings can therefore
be replaced after a transient read/mount error. Losing SSID settings can also
leave the device accessible only via its setup AP.

Required: distinguish first-time provisioning from an existing filesystem
failure; preserve the partition, retry transient errors, retain a validated
fallback configuration, and expose degraded storage without formatting.
Inject read errors, low-memory mount failures, CRC failures and full storage.

### 4. P1: Relay timing depends on blocking configuration/network operations

Evidence: the relay task calls `sc_inputs_poll` after deadline processing
(`components/sc_relay/sc_relay.c:116`). Input polling takes the config mutex
and calls QoS1 publishing (`components/sc_inputs/sc_inputs.c:37,51,106`).
Payload acquisition can wait 250 ms per call, with additional queue waiting.
Configuration persistence holds the config mutex through filesystem I/O
(`components/sc_config/sc_config.c:220,225`). Multiple input updates or a
save can prevent the relay task from reaching its next TTL/TTA deadline check.

Required: keep relay deadline handling independent of flash and MQTT waits.
Publish input changes nonblockingly with a bounded latest-state mailbox and
use small runtime configuration snapshots. Measure worst-case deadline error
with full queues, blackholed MQTT, noisy inputs and simultaneous compaction.

### 5. P1: Retained toggle commands can execute again after reconnect

Evidence: `components/sc_inputs/sc_inputs.c:37` publishes input commands with
retain=true, including `tog` in `input_changed`. The MQTT receiver treats
every `tog` delivery as a fresh toggle (`components/sc_mqtt/sc_mqtt.c:320`)
and subscribes again at reconnect. A retained toggle routed to a relay command
topic can therefore change the held state without a new physical event.
QoS1 duplicate delivery is another non-idempotent-command concern.

Required: retain state, not one-shot toggle events; use idempotent desired
ON/OFF commands or explicit event identities/deduplication. Test duplicate
delivery and resubscription with existing retained command data.

### 6. P1: No application service-progress supervisor

Evidence: `components/sc_health/sc_health.c` records counters/heap but checks
no task heartbeat or service progress. The installed SDK feeds its watchdog
from `esp_internal_idle_hook`; a task blocked on a mutex can leave idle running.
`main/app_main.c:25` onwards also aborts startup if any subsystem fails.
The HTTP panic and HA worker allocation failures in the supplied logs already
show that resource faults are real, rather than only hypothetical.

Required: fixed-memory progress supervision, bounded operations, degraded
startup for optional services, and service-specific recovery for Wi-Fi/MQTT/
HTTP/Modbus. Preserve the last confirmed relay state. A watchdog panic/reset
or scheduled restart does not satisfy zero-reboot acceptance. A stalled relay
task needs prevention/containment, not a board reset disguised as recovery.

## Additional findings

### 7. P2: A transient MQTT startup failure is not retried while Wi-Fi stays up

`components/sc_mqtt/sc_mqtt.c:529` invokes `start_client_if_ready` once on the
IP-ready event and discards its error. If task allocation fails, there is no
periodic start retry in `pubtask`. MQTT can stay offline until another IP event.
Add bounded, observable startup retries that do not recreate the relay task.

### 8. P2: Every changed timing command rewrites the whole configuration

`components/sc_config/sc_config.c:239` persists a 4,528-byte config for every
changed TTL/TTA value. Identical values are correctly skipped. One change per
minute is 525,600 config transactions/year, not 525,600 erase cycles: actual
wear depends on compaction, free space and the fitted flash's endurance.
Separate runtime changes from durable configuration and debounce/coalesce
commits with explicit durability semantics. Measure erase counts; do not
claim a flash lifetime without identifying the physical flash part.

### 9. P2: SDK MQTT deadlines use adjustable wall-clock time

Installed SDK `components/mqtt/esp-mqtt/lib/platform_esp32_idf.c:27` uses
`gettimeofday` for reconnect, keepalive and outbox timestamps. Backward SNTP
steps can delay deadline/expiry checks; forward steps can expire pending work.
Use monotonic time in a pinned, tested SDK patch. Test NTP unavailable at boot,
first synchronization, forward/backward corrections and recovery while MQTT
has pending messages. Local relay deadlines already use `esp_timer_get_time`.

### 10. P2: Modbus timeouts are not absolute transaction deadlines

`components/sc_modbus/sc_modbus.c:80` applies the receive timeout repeatedly
to each partial read. In `serve_client` (line 401), the two-second partial-frame
deadline is checked only on receive timeout, not when bytes keep arriving.
One persistent accepted client also owns the single server loop. Trickle data
can delay other clients and remote polling/writes. Enforce monotonic whole-frame
and transaction deadlines; define the intended single/multiple-client contract.

### 11. P2: Duplicate config entries in restore leak a 6 KiB allocation

`components/sc_web/sc_web.c:1299` assigns a new `malloc(6144)` to `s->config`
each time `/config.json` is encountered without rejecting duplicates or freeing
the old buffer. `backup_restore` frees only the final pointer. A malformed
backup with duplicate config keys can permanently reduce heap until reboot.
Reject duplicates, cap restore input/work, and test failed/repeated restores.
Uploads also truncate the destination directly and ignore close errors; use
temporary files and verified close/rename before reporting success.

## What is already sound, and what remains unknown

- Relay/sensor/remote-age deadlines predominantly use 64-bit monotonic time.
  The application tick arithmetic uses short wrap-safe intervals. The active
  build runs at 100 Hz: 32-bit RTOS ticks wrap after about 497 days, not 49 days.
  This does not prove all SDK counters/paths safe; test rollover explicitly.
- Ordinary sensor telemetry uses a fixed payload pool and QoS0. Configuration
  commits use checksum + temporary file + rename; this is worth preserving.
- Missing temperature readings suppress new control decisions and keep the
  previous GPIO state, consistent with the user's stated hold-last policy.
  Physical power-loss restoration of the last output is not implemented;
  relay initialization currently defaults OFF, which is the specified fallback
  when no output history is available.
- No broad normal-loop heap leak was established. Transient fragmentation,
  QoS outbox growth, malformed restore leaks and unbudgeted stack paths remain.
- External thermal cutouts, fridge compressor anti-short-cycle protection,
  pump dry-run protection and stable power are separate from this uptime audit.
  Holding ON is not a substitute for those load-specific protections.

## Verification performed

All eight existing host/build-report checks passed: HTTP stack budget,
DS18B20, application MQTT pool, relay/MQTT, HA discovery, web/gzip,
temperature page and readings page. The additional SDK-outbox diagnostic
confirmed the ineffective hard limit. The original MQTT persistence arithmetic
found the 64-byte deficit before interrupts; the follow-up stack patch builds
successfully with 704 bytes of budgeted headroom and an old-size rejection test.
The publisher-watermark mock test covers enabled and absent task handles.
The subsequent USB deployment to the matching board on COM3 succeeded, with
all flashed images hash-verified. No full-flash backup was taken, as requested.
The previous IP did not respond afterward and the bounded serial capture was
empty, so runtime operation remains unverified. No 365-day runtime claim
follows from the build, host checks or successful upload.

## Qualification before the unattended mission

1. Fix the release blockers; freeze firmware, SDK, configuration and ELF.
   Add task-stack checks and progress/queue/outbox/socket/erase counters.
2. Fault-inject heap allocation failures, missing ACKs, DNS/AP/broker outages,
   trickle clients, bad storage reads, full flash, sensor disconnects, duplicate
   commands, malformed restores and NTP/tick rollover. Keep outputs under the
   hold-last/OFF-if-unknown policy throughout. No recovery reboot is allowed.
3. Run at least a 90-day qualification on several representative boards,
   including worst configured features, warm enclosure and real network faults.
   This raises confidence; it does not certify a year statistically.
4. Log externally: boot count/reset cause, per-task heartbeat and minimum stack,
   free/minimum/largest heap, SDK outbox bytes/items, queue high watermarks,
   socket counts, sensor freshness, service outage duration and relay deadline
   error. Monitoring must not be the thing that keeps the application alive.
5. Release only with zero unexpected resets, no missed/duplicate control actions,
   bounded memory and deadlines, and complete autonomous service recovery.
   Set numerical outage/latency limits for the actual loads before qualification.

Primary context: [ESP8266 system tasks](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/system-tasks.html)
documents idle-hook watchdog feeding. [littlefs](https://github.com/littlefs-project/littlefs/blob/master/README.md)
documents power-loss resilience and dynamic wear leveling; these do not make
the application's unconditional format-on-error policy safe.
