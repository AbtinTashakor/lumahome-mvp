# LumaHome Troubleshooting

## Firmware does not build

1. Run from `firmware` and verify the environment:

```powershell
pio --version
pio run -e uno_r4_wifi
```

2. Confirm `platformio.ini` uses `board = uno_r4_wifi` and the Renesas Arduino
framework.
3. Copy `include/Secrets.example.h` to `include/Secrets.h`; the real file is
required locally but is Git-ignored.
4. Let PlatformIO install the pinned `ArduinoJson 6.21.5` dependency.
5. If `pio` is not on `PATH`, use the PlatformIO IDE task or its installed CLI.

## Arduino does not connect to Wi-Fi

- Verify SSID/password without printing the password to Serial or source
  control.
- Confirm the access point supports the UNO R4 WiFi; use 2.4 GHz where required
  by the network setup.
- Move closer to the access point and inspect `rssi` with `network`.
- Watch for `wifi: connecting`, retry backoff, and transition messages.
- Rebuild/upload after changing compile-time secrets.

## Wi-Fi connects but backend is offline

- `BACKEND_HOST` must be the computer's LAN IPv4 address, not `localhost` or
  `127.0.0.1`.
- The backend defaults to `0.0.0.0:3000`. If overriding it, keep a LAN-accessible
  address:

```powershell
$env:LUMAHOME_BIND_ADDRESS = "0.0.0.0:3000"
cargo run --manifest-path backend\Cargo.toml
```

- Run `ipconfig`, verify both devices use the same local network, and check that
  `BACKEND_PORT` matches.
- Allow TCP port 3000 through Windows Firewall on the private network.
- The firmware requires an HTTP `Content-Length` response and rejects chunked
  transfer encoding, oversized bodies, non-2xx status, and malformed JSON.
- Use `network` and `heartbeat` to inspect/schedule a transaction.

## Dashboard works locally but Arduino cannot connect

The browser and backend run on the same computer, so the browser can use
`http://127.0.0.1:3000`. The Arduino is a different network host and cannot use
the computer's loopback address. Put the computer's LAN IPv4 address in
`Secrets.h` and verify firewall/routing access.

## Backend does not start

- Verify Rust/Cargo and build first:

```powershell
cargo build --manifest-path backend\Cargo.toml
```

- Check whether port 3000 is already in use.
- The default SQLite database is `backend/data/lumahome.db`; the parent is
  created automatically. Override it with a valid SQLite URL if needed:

```powershell
$env:LUMAHOME_DATABASE_URL = "sqlite://C:/temp/lumahome.db"
```

- Remove environment overrides in a new terminal if an old bind address,
  database URL, or device ID is causing confusion.

## Desired State is ignored

- Compare dashboard desired and firmware `config_version` using `status` and
  `config`.
- Equal and stale versions are intentionally ignored. Change Desired State so
  the backend increments its version.
- Required response types are: mode `manual` or `night`,
  `manual_light_on` boolean, and a non-negative version fitting `uint32_t`.
- Invalid or partial Desired objects cause the complete heartbeat to fail.
- Local Serial overrides intentionally keep the backend version unchanged until
  a newer backend configuration arrives.

## Events remain pending

- Run `events`, `network`, and `heartbeat`.
- Confirm the backend response includes numeric `events_ack_seq` when events
  were submitted; null/missing ACK removes nothing.
- Check backend logs and `GET /api/events?limit=50` for database insertion.
- Batches must contain at most 32 strictly increasing sequences and the exact
  type-specific fields.
- A lost HTTP response intentionally leaves events pending for duplicate-safe
  retry.
- ACK values above the highest sequence sent in that heartbeat are rejected.

## RGB behavior seems reversed

The room-light RGB LED is active HIGH/common cathode:

```text
D9  Red
D10 Blue
D11 Green
HIGH = ON
LOW  = OFF
```

White enables all three channels. Do not invert this logic for the confirmed
shield wiring.

## Night Mode appears unstable

- Run `status` repeatedly and inspect raw/filtered readings.
- The firmware uses an eight-sample moving average sampled every 100 ms.
- Below 250 turns the light on; above 350 turns it off; 250–350 preserves the
  previous output.
- Keep the sensor away from direct RGB feedback and verify ambient positioning.
- Allow roughly the filter window to settle after a large light change.

## Red LED stays on

Red indicates Wi-Fi disconnected or backend unhealthy. Blue is on only when
Wi-Fi is connected and the last heartbeat is valid. An invalid Serial command
also briefly requests Red, but expiration cannot clear an existing network
fault indication.
