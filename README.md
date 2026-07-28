# LumaHome

## Overview

LumaHome is a local-first smart lighting MVP built with Arduino UNO R4 WiFi,
C++ firmware, a Rust/Axum backend, SQLite, and a lightweight RTL web dashboard.
The device keeps its core lighting behavior available when Wi-Fi or the backend
is unavailable, then reconciles state and buffered events after connectivity
returns.

The project is a compact reference implementation, not a production-ready
commercial home automation platform.

## Key Features

- Manual and sensor-driven Night lighting modes
- Non-blocking light sampling with an eight-sample moving average
- Hysteresis thresholds to avoid rapid light switching
- EEPROM-backed mode, manual command, and configuration version
- Automatic Wi-Fi reconnect with bounded exponential backoff
- Desired and reported state synchronization over HTTP heartbeats
- Six-second device-online timeout in the backend
- Fixed-size RAM event queue with retry, acknowledgement, and deduplication
- SQLite persistence for desired state and device events
- Framework-free RTL dashboard with Persian labels and a bundled Vazirmatn font
- Exactly two short startup beeps and no other buzzer output

## Architecture

```text
Browser dashboard
       |
       | same-origin JSON API
       v
Rust/Axum backend ---- SQLite
       ^
       | heartbeat + events / desired state + ACK
       |
Arduino UNO R4 WiFi
  |-- local mode and light control
  |-- sensor filtering and hysteresis
  |-- EEPROM configuration
  `-- RAM event queue
```

The firmware owns real-time lighting behavior. The backend persists operator
intent and event history, while the dashboard displays and changes desired
state. See [architecture.md](docs/architecture.md) for the component and data
flow details.

## Hardware

| Function | Arduino pin |
| --- | --- |
| Status LED, red | D12 |
| Status LED, blue | D13 |
| RGB red | D9 |
| RGB blue | D10 |
| RGB green | D11 |
| Passive buzzer | D5 |
| Light sensor | A1 |

The firmware targets `uno_r4_wifi` through PlatformIO. Confirm voltage,
resistors, LED polarity, and common-anode/common-cathode behavior for the
specific components before wiring.

## Repository Structure

```text
.
|-- backend/                 Rust/Axum API, SQLite migrations, dashboard
|-- firmware/                PlatformIO Arduino firmware
|-- docs/                    Architecture, protocol, tests, and operations
|-- .github/workflows/       Backend and firmware validation
|-- .editorconfig
|-- LICENSE
`-- README.md
```

## Backend Setup

Prerequisites:

- A current stable Rust toolchain
- SQLite support provided through the bundled `sqlx` dependency

From `backend/`:

```powershell
cargo run
```

The default listener is `0.0.0.0:3000`. Open
[http://localhost:3000](http://localhost:3000) for the dashboard. The runtime
database is created at `backend/data/lumahome.db` and is excluded from Git.

To validate the backend:

```powershell
cargo fmt --all -- --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test --all
cargo build
```

## Firmware Setup

Prerequisites:

- Arduino UNO R4 WiFi
- PlatformIO Core or the PlatformIO IDE extension

From `firmware/`:

```powershell
Copy-Item include/Secrets.example.h include/Secrets.h
pio run -e uno_r4_wifi
```

Edit the copied `Secrets.h` with the local Wi-Fi network and the backend
computer's LAN address. The real file is excluded from Git. Upload and monitor
only after checking that the connected board and port are correct:

```powershell
pio run -e uno_r4_wifi -t upload
pio device monitor --baud 115200
```

Hardware upload is not required to build or review the project.

## Configuration

The backend accepts these optional environment variables:

| Variable | Default | Purpose |
| --- | --- | --- |
| `LUMAHOME_BIND_ADDRESS` | `0.0.0.0:3000` | HTTP listener address |
| `LUMAHOME_DATABASE_URL` | `sqlite://data/lumahome.db` | SQLite connection URL |
| `LUMAHOME_DEVICE_ID` | `lumahome-01` | Accepted device identity |

Safe placeholders are provided in [backend/.env.example](backend/.env.example)
and [firmware/include/Secrets.example.h](firmware/include/Secrets.example.h).
The backend does not automatically load `.env`; set variables in the process
environment or use a local process manager.

## API Overview

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/health` | Backend health |
| `GET` | `/api/state` | Desired state, reported state, and online status |
| `GET` | `/api/events?limit=5` | Recent device events |
| `POST` | `/api/mode` | Set `manual` or `night` desired mode |
| `POST` | `/api/light` | Set the manual desired light command |
| `POST` | `/api/device/heartbeat` | Report device state and queued events |

The heartbeat response includes current desired state, the next heartbeat
interval, and the highest accepted event sequence. The complete JSON contract
and validation rules are in [protocol.md](docs/protocol.md).

## Offline and Recovery Behavior

- Manual and Night modes continue locally without Wi-Fi or a backend.
- EEPROM restores the last valid mode, manual command, and applied backend
  configuration version after reset.
- Wi-Fi reconnect retries use increasing delays capped at 30 seconds.
- Failed heartbeats do not change the current lighting output.
- Events remain in a fixed 64-entry RAM queue and are retried in batches of up
  to 32 until acknowledged.
- SQLite deduplicates events by device ID, boot ID, and sequence number.
- Pending RAM events are intentionally lost on power loss or reset; persistent
  lighting configuration is retained.

## Running the Demo

1. Start the backend from `backend/` with `cargo run`.
2. Open `http://localhost:3000` and confirm backend health.
3. Power the configured Arduino and confirm two short startup beeps.
4. Exercise Manual and Night modes from Serial or the dashboard.
5. Disconnect Wi-Fi or stop the backend and confirm local lighting continues.
6. Restore connectivity and confirm heartbeat recovery, event acknowledgement,
   and dashboard history.

The detailed sequence is in [demo-guide.md](docs/demo-guide.md). Failure
scenarios and expected results are in
[failure-tests.md](docs/failure-tests.md).

## Validation

The repository includes two GitHub Actions workflows:

- Backend: formatting, Clippy with warnings denied, tests, and build
- Firmware: PlatformIO build for the configured UNO R4 WiFi environment

The latest documented results and hardware-test boundaries are in
[validation-summary.md](docs/validation-summary.md). Operational help is in
[troubleshooting.md](docs/troubleshooting.md).

## Project Scope

This MVP focuses on:

- Local-first behavior
- Offline operation
- State synchronization
- Network recovery
- Embedded persistence
- Event delivery with retry and deduplication

It intentionally omits authentication, TLS, fleet management, durable
device-side event storage, remote deployment, and commercial-device hardening.

## License

Licensed under the [MIT License](LICENSE).
