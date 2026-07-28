# LumaHome Firmware

## Stage 1: Local Hardware Foundation

Stage 1 provides local lighting operation for the Arduino UNO R4 WiFi and
B04505 shield. It includes Manual and Night modes, an eight-sample moving
average light sensor filter, 250/350 hysteresis, active-high RGB room-light
control, status LEDs, and a non-blocking serial command interface.

The supported commands are:

```text
help
status
mode manual
mode night
light on
light off
config
network
heartbeat
events
```

## Stage 2: Persistent Local Configuration

Stage 2 stores only the current mode and stored manual-light command in the UNO
R4's built-in EEPROM-compatible data-flash storage. The room-light output,
sensor readings, status LEDs, parser state, and timers are never persisted.

The fixed 12-byte version 1 record contains a LumaHome magic value, record
version, mode, manual-light boolean, reserved byte, and a 32-bit FNV-1a
checksum. Invalid, corrupt, missing, or unsupported records fall back safely to
Manual Mode with the stored manual command off. A valid default record is then
written once.

Runtime configuration changes are marked dirty and saved after approximately
750 ms without another change. A later change restarts that interval, so quick
commands are coalesced. Unchanged commands, sensor samples, and automatic Night
Mode transitions do not request saves. Saves are read back and validated; a
failed verification leaves the configuration dirty and retries after five
seconds without stopping local lighting.

On startup, restored Manual Mode immediately applies the stored manual command.
Restored Night Mode initially leaves the RGB light off, then evaluates the
sensor as soon as the first non-blocking sample is available.

The Renesas EEPROM API commits `EEPROM.put()` implicitly and synchronously; it
does not expose a direct success result, asynchronous operation, or separate
`commit()` call. LumaHome detects failures by reading and validating the saved
record afterward. The application performs no heap allocation, although the
installed core's EEPROM implementation internally allocates a temporary flash
page buffer while writing.

Wi-Fi, HTTP, JSON, backend communication, heartbeat, event buffering, and
offline synchronization are not implemented.

## Build and Monitor

From the `firmware` directory:

```text
pio run
pio device monitor --baud 115200
```

Enter `help` to list the commands. Enter `config` to inspect boot-time storage
validity, record version, dirty state, restored mode, and restored manual-light
command.

## Stage 2 Hardware Acceptance Tests

### Test 1 — Default Boot

Boot with empty or invalid storage. Confirm Serial reports that defaults were
used and the status shows:

```text
mode=manual
manual_light_on=false
actual_light_on=false
```

The RGB light must be off.

### Test 2 — Restore Manual ON

Send:

```text
mode manual
light on
```

Wait at least 750 ms and use `status` until `config_dirty=false`. Reset or
power-cycle the board. Confirm Manual Mode, `manual_light_on=true`,
`actual_light_on=true`, and a white RGB light.

### Test 3 — Restore Manual OFF

Send `light off`, wait until `config_dirty=false`, then reset. Confirm Manual
Mode, both light fields false, and the RGB light off.

### Test 4 — Restore Night Mode

Send:

```text
light on
mode night
```

Wait until `config_dirty=false`, then reset. Confirm `mode=night` and
`manual_light_on=true`. The stored manual command must not control the RGB
light. Cover the sensor and confirm a filtered value below 250 turns it white;
shine a flashlight and confirm a value above 350 turns it off.

### Test 5 — Stored Manual Command After Night Mode

While in Night Mode, send `light off`, wait until `config_dirty=false`, and
reset. Confirm Night Mode and `manual_light_on=false`. Send `mode manual` and
confirm `actual_light_on=false` with the RGB light off.

### Test 6 — Deferred Save

Send quickly:

```text
light on
light off
light on
```

Confirm local responses remain immediate, configuration becomes dirty, and
only one `config: saved` message appears about 750 ms after the final change.
Reset and confirm `manual_light_on=true`.

### Test 7 — Unchanged Commands

When already in Manual Mode with the manual light on, repeatedly send:

```text
mode manual
light on
mode manual
light on
```

Confirm `config_dirty=false` and that no new `config: saved` message appears.

### Test 8 — Night Transitions Do Not Save

Enter Night Mode and alternate the sensor between readings below 250 and above
350. Confirm the RGB output follows the sensor while no configuration save is
reported.

### Test 9 — Serial Responsiveness

Change a setting and repeatedly send `status` while the deferred save is
pending. Confirm Serial remains responsive, sensor sampling continues, Night
Mode remains operational, and the save completes without a visible pause.

## Stage 3: Local-First Backend Synchronization

Stage 3 added Wi-Fi reconnection, device heartbeats, validated Desired State,
backend health tracking, and network status LEDs. Local Manual and Night modes,
sensor sampling, Serial commands, and deferred persistence continue operating
when Wi-Fi or the backend is unavailable. Its heartbeat transport is retained
by Stage 4; the `events` array is empty whenever no events are pending.

`NetworkManager` starts and monitors Wi-Fi attempts. Attempts have a 10-second
application window and retries use 2, 4, 8, 16, then 30-second backoff. Because
the installed WiFiS3 `WiFi.begin()` implementation polls internally, that
internal polling timeout is set to zero. The connection command is issued once
and the application state machine checks its progress while local work runs.

`BackendClient` schedules heartbeats, constructs bounded HTTP/1.1 requests,
accepts only 2xx responses with a valid bounded `Content-Length`, parses JSON,
and returns a fully validated Desired State. It uses a 256-byte request buffer,
a maximum 768-byte response body, stack-backed ArduinoJson documents, and a
1.5-second overall connection/response deadline. WiFiClient calls are
synchronous, so a failed transaction can pause one loop iteration for up to
approximately 1.5 seconds; there are no delays or retry loops around it.

The heartbeat request is:

```json
{
  "device_id": "lumahome-01",
  "boot_id": 4182,
  "mode": "night",
  "light_on": true,
  "sensor_raw": 280,
  "config_version": 3,
  "events": []
}
```

The expected response shape is:

```json
{
  "desired": {
    "mode": "night",
    "manual_light_on": false,
    "config_version": 4
  },
  "heartbeat_interval_ms": 2000,
  "events_ack_seq": null
}
```

The entire Desired object is validated before use. Only a version newer than
the device's persisted backend `configVersion` is applied. Equal and stale
versions cannot overwrite local state. Valid heartbeat intervals from 1000 to
30000 ms replace the current interval; missing or invalid intervals leave it
unchanged. Stage 4 adds the ACK processing described below.

Serial commands are local overrides until a newer backend `config_version`
arrives. Local commands persist their mode and manual command but never
increment `configVersion` and never create backend write requests.

The persistence record is now version 2 and adds the backend configuration
version. A valid Stage 2 version-1 record is migrated by restoring its mode and
manual command, assigning `configVersion=0`, and scheduling a deferred rewrite
as version 2. Invalid storage still uses the safe Manual/off/version-zero
defaults.

Network LEDs use these states:

- Blue on and red off: Wi-Fi connected and the last heartbeat was healthy.
- Blue off and red on: startup, Wi-Fi disconnected, or backend unavailable.
- Invalid commands also request the red indication; an existing network-fault
  indication is never cleared when that temporary indication expires.

The backend becomes online only after a 2xx heartbeat with valid HTTP framing,
valid JSON, and a complete valid Desired object. Any heartbeat failure marks it
offline without changing local lighting state.

### Secrets setup

1. Copy `include/Secrets.example.h` to `include/Secrets.h`.
2. Set `WIFI_SSID`, `WIFI_PASSWORD`, `BACKEND_HOST`, and `BACKEND_PORT`.
3. Configure the backend to listen on the LAN interface, not only `127.0.0.1`.
4. Allow the backend port through Windows Firewall for the local network.

`BACKEND_HOST` must be the computer's LAN address, such as `192.168.1.100`.
The Arduino cannot reach a backend through the computer's `localhost` address.
`Secrets.h` is Git-ignored; `Secrets.example.h` contains only placeholders.

## Stage 3 Hardware Acceptance Tests

### Test 1 — Boot Without Wi-Fi

Use an invalid SSID or turn off the router. Confirm normal boot, red on, blue
off, working Manual/Night modes and Serial commands, and continuing reconnect
attempts with increasing backoff.

### Test 2 — Wi-Fi Connected, Backend Offline

Enable Wi-Fi with the backend stopped. Confirm Wi-Fi connects, the backend
remains offline, red stays on, blue stays off, and heartbeat failures do not
alter the mode or light.

### Test 3 — Backend Becomes Available

Start the backend while the firmware is running. Confirm the next heartbeat
succeeds, `backend: online` is printed, red turns off, and blue turns on without
resetting the board.

### Test 4 — Heartbeat Payload

Inspect backend logs and confirm the heartbeat contains the current
`device_id`, boot ID, lowercase mode, actual light output, raw sensor value,
persisted backend configuration version, and an `events` array. It is empty
when no Stage 4 transitions are pending.

### Test 5 — Apply Newer Desired State

Return:

```json
{"desired":{"mode":"manual","manual_light_on":true,"config_version":1},"heartbeat_interval_ms":2000,"events_ack_seq":null}
```

Confirm version 1 is applied, Manual Mode turns the RGB white, configuration is
briefly dirty, and `config: saved` appears.

### Test 6 — Ignore Equal Version

Return version 1 again with different mode or light values. Confirm the current
mode and light remain unchanged.

### Test 7 — Ignore Stale Version

Return `config_version=0`. Confirm it is ignored and no persistence save occurs.

### Test 8 — Apply Newer Night Mode

Return:

```json
{"desired":{"mode":"night","manual_light_on":false,"config_version":2},"heartbeat_interval_ms":2000,"events_ack_seq":null}
```

Confirm version 2 is applied and Night Mode immediately uses a valid sensor
sample. Darkness below 250 turns RGB on; brightness above 350 turns it off.

### Test 9 — Restart Persistence

After version 2 is saved, stop the backend and reset. Confirm restored Night
Mode, `manual_light_on=false`, `config_version=2`, and local sensor control.

### Test 10 — Backend Restart

While online, stop the backend. After the failed heartbeat confirm red on, blue
off, and uninterrupted local operation. Restart the backend and confirm the next
heartbeat restores blue on and red off without an Arduino reset.

### Test 11 — Wi-Fi Disconnect

Turn off the router while online. Confirm local operation, red on, and blue off.
Restore Wi-Fi and confirm automatic reconnection, resumed heartbeat, and backend
online recovery.

### Test 12 — Invalid JSON

Return invalid JSON or an incomplete Desired object. Confirm no state change or
persistence write, backend offline status, and continued local operation.

### Test 13 — Local Override

After accepting version 2, send `mode manual` and `light on`. Confirm local
state changes and is persisted while `config_version` remains 2. A backend
response at version 2 must not overwrite it; a valid response at version 3
must.

### Test 14 — Serial Responsiveness

While disconnected, reconnecting, saving, and heartbeating, repeatedly send
`status`, `network`, and `config`. Confirm sensor readings and Night Mode keep
updating and Serial remains usable apart from the documented bounded synchronous
HTTP transaction duration.

## Stage 4: Reliable RAM Event Delivery

Stage 4 records `mode_changed`, `light_changed`, and `wifi_changed` transitions
in a fixed 64-entry RAM ring buffer. Every event is a pointer-free 12-byte
record containing a per-boot sequence, `millis()` uptime, compact event type,
and encoded value. The full ring buffer object occupies 792 bytes.

Sequence numbers start at 1 after each boot and increase for every new event.
They are never reassigned during retries and are not reset after ACK. The
delivery identity is:

```text
device_id + boot_id + seq
```

The backend schema discovered in its Rust `DeviceEvent` enum is:

```json
{"seq":1,"uptime_ms":100,"type":"mode_changed","mode":"night"}
{"seq":2,"uptime_ms":200,"type":"light_changed","light_on":true}
{"seq":3,"uptime_ms":300,"type":"wifi_changed","connected":false}
```

Each variant rejects unknown fields and fields belonging to another variant.
The backend accepts at most 32 strictly increasing events per heartbeat, stores
the batch transactionally, deduplicates on `(device_id, boot_id, seq)`, and
returns the highest sequence from the accepted batch as `events_ack_seq`.
Sequence gaps are valid as long as the batch remains increasing.

Heartbeats serialize at most the 32 oldest pending events in chronological
order. A fixed 4096-byte request buffer provides a bounded exact
`Content-Length`; response parsing retains the 768-byte body limit and 384-byte
`StaticJsonDocument`. Events remain queued after HTTP failure, invalid
responses, null or missing ACK, and lost responses. Retries reuse the original
boot ID and sequence.

A valid ACK is processed only after the existing HTTP, JSON, and Desired State
validation succeeds. ACK values must fit `uint32_t` and may not exceed the
highest sequence included in that heartbeat. Valid partial ACKs remove only
events with `seq <= events_ack_seq`; stale and duplicate ACKs are idempotent.

When the ring buffer is full, the oldest pending event is dropped and the new
event is appended. The per-boot dropped count increases and sequence numbers
are not reused, so overflow may produce sequence gaps. Warnings are rate-limited
to avoid noisy Serial output.

Delivery is at least once while the current boot remains powered and the event
remains in the RAM ring buffer. The backend provides duplicate suppression.
Events and ACK state are not persisted: a reset loses pending events, generates
a new boot ID, empties the queue, and starts sequence numbering at 1. Persistent
mode, manual command, and backend configuration version still restore normally.

Events are created only after boot initialization:

- `mode_changed` when a local command or newer Desired State changes mode.
- `light_changed` whenever actual RGB output changes, including Night Mode
  sensor transitions and its first valid post-boot evaluation.
- `wifi_changed` on genuine connected and disconnected transitions.

Repeated commands, restored configuration, unchanged hysteresis decisions,
manual-command changes that do not alter Night Mode output, retry attempts, and
backend online transitions do not create duplicate events. Event handling never
affects status LEDs or EEPROM.

Use `events` for compact queue diagnostics. `status` also reports
`events_pending` and `events_dropped`. No arbitrary event-injection command is
provided. Stage 5 functionality is not implemented.

## Stage 4 Hardware and Integration Acceptance Tests

### Test 1 — Empty Buffer at Boot

Reset and immediately run `events` before a connection transition occurs.
Confirm `events_pending=0`, `dropped=0`, and `next_seq=1`. No restored-state or
synthetic light event should exist.

### Test 2 — Local Mode Event

While in Manual Mode, send `mode night` followed by `events`. Confirm one
`mode_changed` event; a `light_changed` event may follow if Night evaluation
changes RGB output. Send `mode night` again and confirm no duplicate mode event.

### Test 3 — Manual Light Event

Send `mode manual`, `light on`, and `events`. Confirm `light_changed=true` only
if RGB changed from off to on. Repeat `light on` and confirm no duplicate.

### Test 4 — Night Sensor Events

Enter Night Mode, cover the sensor until filtered input is below 250, and
confirm a light-on event. Shine a flashlight until input is above 350 and
confirm a light-off event. Readings between 250 and 350 must create none.

### Test 5 — Wi-Fi Transition Events

With Wi-Fi connected, turn off the router, wait for disconnection detection,
and run `events`; confirm `wifi_changed=false`. Restore Wi-Fi and confirm one
`wifi_changed=true`. Retry attempts must not create duplicate false events.

### Test 6 — Offline Queueing

Stop the backend or Wi-Fi and send `mode manual`, `light on`, and `mode night`,
then cover and illuminate the sensor. Confirm pending events increase, none are
removed, and local lighting remains responsive.

### Test 7 — Successful Synchronization

Restore Wi-Fi and backend. Confirm each heartbeat sends up to 32 oldest events,
the backend stores them and returns an ACK, and pending count eventually reaches
zero without an Arduino reset.

### Test 8 — Lost ACK Retry

Let the backend receive a heartbeat but interrupt its response. Confirm pending
events remain and the next heartbeat resends identical boot IDs and sequences.
After a valid ACK, confirm removal and only one backend row per identity.

### Test 9 — Partial ACK

Create multiple events and return an ACK for an earlier sent sequence. Confirm
only events through that sequence are removed and later events are resent.

### Test 10 — More Than 32 Events

Generate more than 32 events while offline. Restore the backend and confirm the
first request contains only the oldest 32; after ACK, later events arrive in the
next batch. No request may exceed 32 events.

### Test 11 — Buffer Overflow

Generate more than 64 events while offline. Confirm no crash, oldest-event
drops, increasing `events_dropped`, continued recording, and responsive local
lighting.

### Test 12 — Reset Loss Limitation

Create pending offline events and reset before synchronization. Confirm the
persistent configuration restores, boot ID changes, pending count becomes zero,
and `next_seq=1`. The prior RAM-only events are intentionally lost.

### Test 13 — Backend Desired Events

Apply a newer Desired State that changes mode and actual light. Confirm exactly
one mode event and, only if RGB changes, one light event. Confirm normal
deferred configuration persistence.

### Test 14 — Serial Responsiveness

During pending-event retries, repeatedly send `status`, `events`, `network`, and
`config`. Confirm Serial, sensor sampling, Night Mode, deferred EEPROM saves,
and ring-buffer diagnostics remain operational outside the documented bounded
HTTP transaction.

## Stage 5: Final Firmware Validation

All five MVP stages are complete. The final firmware architecture
keeps hardware output (`LightController`), sensor filtering (`LightSensor`),
mode logic (`ModeController`), configuration persistence (`ConfigStore`), Wi-Fi
reconnect (`NetworkManager`), heartbeat protocol (`BackendClient`), and the RAM
event queue (`EventBuffer`) in focused components. `main.cpp` coordinates
observable transitions, Serial commands, status feedback, Desired State, and
ACK processing.

Final build command:

```powershell
pio run -e uno_r4_wifi
```

Validated resource usage:

```text
Flash: 68,668 / 262,144 bytes (26.2%)
RAM:   10,244 / 32,768 bytes (31.3%)
```

The Stage 1–4 real-hardware and integration procedures are preserved in this
guide. The final validation repeated the firmware build and static
safety/protocol audits; hardware upload and physical fault injection remain
separate operator-run steps.

Known UNO R4 limitation: WiFiS3 and `WiFiClient` contain synchronous internals.
Reconnect submission is application-scheduled without a connection polling
loop, but one heartbeat is still a bounded synchronous transaction of roughly
1.5 seconds. Responses must use `Content-Length`; chunked transfer is rejected.

Project-level setup, architecture, failure testing, demo instructions, and
final evidence are linked from the [root README](../README.md) and the
[`docs`](../docs/) directory.
