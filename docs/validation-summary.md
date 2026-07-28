# LumaHome MVP Validation Summary

Validation date: 2026-07-28

## Components and Results

| Component | Validation | Result |
| --- | --- | --- |
| Firmware | PlatformIO UNO R4 WiFi release build | PASS |
| Firmware behavior | Pins, timing, modes, persistence, reconnect, heartbeat, event queue, ACK, startup buzzer | PASS — static inspection |
| Backend formatting | `cargo fmt --all -- --check` | PASS |
| Backend lint | `cargo clippy --all-targets --all-features -- -D warnings` | PASS |
| Backend tests | `cargo test --all` | PASS — 42/42 |
| Backend build | `cargo build` | PASS |
| Dashboard JavaScript | `node --check backend/static/app.js` | PASS |
| Dashboard runtime | Desktop and 390×844 mobile browser smoke tests | PASS |
| Physical fault matrix | Hardware and network fault injection | NOT RUN |

## Commands Executed

From `backend/`:

```powershell
cargo fmt --all -- --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test --all
cargo build
```

From `firmware/`:

```powershell
python -m platformio run -e uno_r4_wifi
```

Dashboard syntax:

```powershell
node --check backend/static/app.js
```

The dashboard was also loaded through the running Axum backend. The smoke test
confirmed Persian RTL content, bundled Vazirmatn font use, expected control and
status elements, API-backed state/event rendering, no horizontal overflow at
desktop or mobile width, and no browser console warnings or errors.

## Firmware Resource Usage

```text
Target: Arduino UNO R4 WiFi / Renesas RA4M1
Flash: 68,668 / 262,144 bytes (26.2%)
RAM:   10,244 / 32,768 bytes (31.3%)
```

PlatformIO resolved:

- Renesas RA platform 1.9.0
- Arduino Renesas UNO framework 1.6.0
- ArduinoJson 6.21.5
- Built-in EEPROM 1.0
- Built-in WiFiS3

## Protocol Audit

Firmware and backend agree on:

- fixed default device ID `lumahome-01`;
- `POST /api/device/heartbeat` request and response fields;
- lowercase Manual and Night mode values;
- monotonic backend `config_version` semantics;
- the three strongly typed event variants;
- at most 32 strictly increasing events per heartbeat;
- event identity `(device_id, boot_id, seq)`;
- transactional insertion and highest-submitted-sequence acknowledgement;
- duplicate-safe retry behavior.

No protocol incompatibility was found.

## Repository and Secret Audit

- The real `firmware/include/Secrets.h` is ignored.
- `Secrets.example.h` contains safe placeholders.
- Runtime `.env` files, SQLite databases and sidecars, `.pio`, `target`,
  editor settings, logs, and temporary files are ignored.
- No runtime database, real secret header, access token, build output, or
  machine-specific editor file is tracked.
- Repository content and tracked filenames contain no restricted project
  framing.

## Known Environment Warning

Cargo prints `could not canonicalize path C:\Users\Abtin\Desktop\LoopLight` in
this Windows workspace. All Cargo commands exit successfully, Clippy passes
with warnings denied, and the warning is not emitted by LumaHome source code.

## Known Limitations

- Single device with compile-time Wi-Fi and backend configuration.
- Plain HTTP without authentication or TLS.
- No provisioning, OTA update, or multi-device model.
- WiFiS3 and `WiFiClient` contain synchronous internals; a heartbeat is bounded
  to approximately 1.5 seconds but is not asynchronous.
- HTTP responses require `Content-Length`; chunked transfer is unsupported.
- Event queue and ACK state are RAM-only and are lost on reset.
- Oldest events are dropped when the 64-entry queue is full.
- Hardware upload and physical failure injection are not part of this software
  validation run.
