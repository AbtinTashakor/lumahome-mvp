# LumaHome Demo Guide

Target duration: 5–8 minutes. Prepare `firmware/include/Secrets.h`, the backend
database, firewall permission, and a browser tab before the walkthrough.

## Pre-demo startup

From the repository root in PowerShell:

```powershell
Copy-Item firmware\include\Secrets.example.h firmware\include\Secrets.h
ipconfig
```

Edit `Secrets.h` with the 2.4 GHz Wi-Fi credentials and the computer's LAN IPv4
address—not `localhost`. Then:

```powershell
cargo run --manifest-path backend\Cargo.toml
```

The backend binds to `0.0.0.0:3000` by default and serves both the API and
dashboard. Open `http://127.0.0.1:3000/` on the computer. Build/upload and open
the monitor from `firmware`:

```powershell
pio run -t upload
pio device monitor --baud 115200
```

If upload is not part of the live demo, upload beforehand and only open the
monitor. Confirm Windows Firewall allows TCP port 3000 on the private network.

## 1. Project introduction — 30 seconds

“LumaHome is a compact smart-lighting MVP, not a commercial smart
home product. It demonstrates embedded C++, local-first control, fixed-memory
design, Rust/Axum APIs, SQLite persistence, a vanilla JavaScript dashboard, and
recoverable device/backend synchronization.”

## 2. Architecture overview — 45 seconds

- The Arduino owns physical behavior: sensor filtering, hysteresis, RGB output,
  mode logic, configuration persistence, and the RAM event queue.
- The backend owns Desired State, recent reported state, online detection, and
  durable deduplicated event history.
- The dashboard changes Desired State; firmware learns it from heartbeat
  responses rather than receiving direct hardware commands.
- Accepted configuration survives reset. Event delivery is at least once while
  pending data remains in RAM during the current boot.

Use the diagrams in [architecture.md](architecture.md) while explaining this.

## 3. Normal online demonstration — 90 seconds

1. Show the backend terminal and dashboard.
2. Power/reset the Arduino and show the printed boot ID.
3. Wait for Blue on/Red off and “Device Online” in the dashboard.
4. Select Manual Mode in the dashboard.
5. Turn the room light on and off; point out Desired versus reported/actual
   state and the configuration version.
6. Select Night Mode.
7. Cover the sensor; after filtering settles, show RGB white below 250.
8. Shine a flashlight; show RGB off above 350.
9. Explain that readings from 250 through 350 preserve the previous output.

Useful Serial checks:

```text
status
config
network
events
```

## 4. Local-first failure demonstration — 90 seconds

1. Keep Night Mode active.
2. Stop the backend with Ctrl+C, or disconnect Wi-Fi.
3. Show Blue off/Red on and the dashboard eventually reporting offline or
   unavailable.
4. Cover and uncover the sensor. Show that RGB still follows local hysteresis.
5. Use local commands to create observable transitions:

```text
mode manual
light on
light off
mode night
events
```

6. Point out increasing pending sequences and explain that events consume fixed
   RAM rather than blocking the lighting loop.

## 5. Recovery demonstration — 60 seconds

1. Restart the backend with:

```powershell
cargo run --manifest-path backend\Cargo.toml
```

2. If Wi-Fi was disabled, restore it.
3. Show automatic recovery without an Arduino reset.
4. Show Blue on/Red off.
5. Run `events` until pending returns to zero.
6. Show the dashboard’s deduplicated recent event history.
7. Explain that a lost response causes retry with the same
   `(device_id, boot_id, seq)` identity.

The `heartbeat` Serial command can make one heartbeat immediately eligible:

```text
heartbeat
```

## 6. Reset persistence demonstration — 60 seconds

1. Apply a Desired State from the dashboard.
2. Use `status` until `config_dirty=false`.
3. Stop the backend.
4. Reset the Arduino.
5. Run:

```text
status
config
events
```

6. Show the restored mode, manual command, and configuration version.
7. Explain that the boot ID changes, event sequence returns to 1, and pending
   RAM-only events are intentionally lost.

## 7. Technical closing — 30 seconds

Summarize local-first behavior, deterministic memory, rollover-safe scheduling,
versioned Desired State, EEPROM migration, at-least-once event delivery, ACK
validation, and database deduplication. Close by separating deliberate MVP
constraints from production requirements.

## Serial command reference

```text
help
status
config
network
events
heartbeat
mode manual
mode night
light on
light off
```

## Design talking points

### Why Local-First?

Lighting is a physical function that should remain useful when Wi-Fi, the
backend, or the dashboard fails. Network services synchronize intent and
history; they do not sit in the critical path of local control.

### Why HTTP instead of MQTT?

HTTP kept this single-device MVP inspectable and integrated naturally with the
existing Rust API. MQTT could be a better choice for a larger fleet, retained
commands, broker-based fan-out, or lower-overhead telemetry.

### Why `config_version`?

It prevents delayed, duplicated, equal, or stale heartbeat responses from
overwriting a newer accepted configuration. Mode, manual command, and version
are validated and applied as one configuration.

### Why persist configuration but not events?

Operational configuration must survive power loss. Durable events would add
flash-wear, queue-recovery, and transaction-design concerns. For this MVP, a
bounded RAM queue demonstrates outage recovery while keeping persistence small.

### Why a ring buffer?

It gives constant, known memory usage and O(1) append/front access. When full,
dropping the oldest event preserves the most recent operational history and is
visible through diagnostics.

### What delivery guarantee exists?

At-least-once delivery while the event remains in RAM during the current boot.

### How are duplicates prevented?

Firmware retries the same identity. SQLite enforces uniqueness on:

```text
device_id + boot_id + seq
```

### Why is this not exactly once?

A backend commit can succeed while the ACK response is lost, forcing a retry.
Exactly-once claims require durable coordination across both participants. This
design instead uses at-least-once transport and idempotent backend insertion.

### What happens if the event buffer fills?

The oldest pending event is dropped, the newest is appended, and the dropped
counter increases. Sequence numbers are never reused, so gaps can appear.

### What happens when the Arduino resets?

Persistent mode/manual/version restore. A new boot ID is generated, sequence
starts at 1, and unacknowledged RAM events are lost.

### Main production limitations

- One fixed device and compile-time Wi-Fi credentials.
- Plain HTTP with no device authentication or TLS.
- A bounded but synchronous HTTP call.
- RAM-only events, no OTA, no clock synchronization, and no remote provisioning.
- No long-term device-side event persistence or production enclosure/electrical
  certification.

### What would you add for production?

Device authentication, TLS, secure provisioning, OTA, a durable event queue,
watchdog strategy, metrics, structured logging, multi-device support, schema
evolution, automated hardware/integration tests, and MQTT or another broker if
fleet requirements justify it. Hardware enclosure and electrical validation
would also become formal workstreams.
