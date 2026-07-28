# Protocol

This document describes the implemented LumaHome HTTP/JSON API. All API responses, including errors, use JSON.

## Dashboard and Health APIs

### `GET /api/health`

Returns backend health:

```json
{
  "status": "ok",
  "service": "lumahome-backend"
}
```

### `GET /api/state`

Returns desired state, the latest runtime-only report, `last_seen`, and calculated online status. Before the first heartbeat after a backend start, `reported` and `last_seen` are `null` and `online` is `false`. Desired values survive restarts; reported state, `last_seen`, and online status do not.

The device is online through six seconds after its most recent valid heartbeat. Timeout does not discard the last runtime report.

### `POST /api/mode`

Accepts `{"mode":"manual"}` or `{"mode":"night"}` and returns the complete dashboard state. A changed value increments `config_version`; an unchanged value is idempotent.

### `POST /api/light`

Accepts `{"on":true}` or `{"on":false}` in desired Manual mode and returns the complete dashboard state. Night mode returns `409 Conflict` with `manual_control_unavailable`.

## Device Heartbeat

### `POST /api/device/heartbeat`

The single configured device sends its applied state and an optional ordered event batch:

```json
{
  "device_id": "lumahome-01",
  "boot_id": 4182,
  "mode": "night",
  "light_on": true,
  "sensor_raw": 280,
  "config_version": 3,
  "events": [
    {
      "seq": 21,
      "uptime_ms": 8100,
      "type": "mode_changed",
      "mode": "night"
    },
    {
      "seq": 22,
      "uptime_ms": 8400,
      "type": "light_changed",
      "light_on": true
    },
    {
      "seq": 23,
      "uptime_ms": 12000,
      "type": "wifi_changed",
      "connected": false
    }
  ]
}
```

Top-level fields:

- `device_id` must equal the configured non-empty device ID, which defaults to `lumahome-01`.
- `boot_id` is an unsigned integer identifying the current device boot session. It applies to every event in the request; events do not contain their own `boot_id`.
- `mode` is `manual` or `night`.
- `light_on` is the actual output state.
- `sensor_raw` is an unsigned 16-bit sensor value.
- `config_version` is the desired configuration version applied by the device.
- `events` is optional. Omitting it behaves exactly like an empty array.

### Event Schemas

Only these strongly typed, lowercase snake-case variants are accepted.

Mode changed:

```json
{
  "seq": 21,
  "uptime_ms": 8100,
  "type": "mode_changed",
  "mode": "night"
}
```

Light changed:

```json
{
  "seq": 22,
  "uptime_ms": 8400,
  "type": "light_changed",
  "light_on": true
}
```

Wi-Fi changed:

```json
{
  "seq": 23,
  "uptime_ms": 12000,
  "type": "wifi_changed",
  "connected": false
}
```

`seq` must be greater than zero, `uptime_ms` must be unsigned, and a batch must be strictly ordered by increasing `seq`. Duplicate sequence numbers inside one request are invalid. The maximum batch size is 32. Unknown types, missing or incorrect variant fields, negative unsigned values, and invalid field types return JSON `400 Bad Request`.

### Persistence, Retry, and Acknowledgement

Event identity is `device_id + boot_id + seq`. The backend validates the complete request, inserts the supplied events in one SQLite transaction, and relies on a database uniqueness constraint for retry deduplication. It commits before updating runtime reported state and `last_seen`.

After commit, the response contains the current consistent desired-state snapshot and the highest submitted event sequence:

```json
{
  "desired": {
    "mode": "night",
    "manual_light_on": false,
    "config_version": 4
  },
  "heartbeat_interval_ms": 2000,
  "events_ack_seq": 23
}
```

When no events are supplied, `events_ack_seq` is `null`. If the response is lost, the device may retry the same batch. Already-stored identities are successful no-ops, and a duplicate-only batch still acknowledges its highest submitted sequence. The acknowledgement applies only to the heartbeat's `boot_id`.

If any event write or commit fails, SQLite rolls back the whole batch. The backend returns `500` with `persistence_failed`, does not acknowledge events, and does not update reported state or `last_seen`.

## Recent Events

### `GET /api/events`

Returns the 10 newest persisted events by default. `GET /api/events?limit=N` accepts `N` from 1 through 50. Invalid or non-integer values return JSON `400` with `invalid_event_limit`. No events returns `{"events":[]}`.

```json
{
  "events": [
    {
      "device_id": "lumahome-01",
      "boot_id": 4182,
      "seq": 23,
      "uptime_ms": 12000,
      "type": "wifi_changed",
      "connected": false,
      "received_at": "2026-07-22T10:30:00.000Z"
    }
  ]
}
```

Each result contains only its variant value: `mode`, `light_on`, or `connected`. Internal database IDs are not exposed.

## Error Behavior

All API errors use:

```json
{
  "error": {
    "code": "stable_error_code",
    "message": "An understandable description."
  }
}
```

Relevant event codes include `invalid_event_batch`, `event_batch_too_large`, `invalid_event_limit`, `invalid_json`, and `persistence_failed`. An unexpected device returns `403` with `device_not_allowed`. Unsupported top-level mode returns `400` with `unsupported_mode`. Unknown API routes and unsupported methods also return JSON errors.

## Firmware behavior

The Arduino UNO R4 WiFi firmware implements this protocol with fixed-capacity
buffers. It creates one `uint32_t` boot ID per boot, starts event sequence at 1,
sends at most the oldest 32 of 64 RAM events, and retries unacknowledged entries
with their original identity. A numeric ACK is accepted only after the HTTP and
JSON response is valid and only when it does not exceed the highest sequence
included in that request.

Desired State is applied only when `desired.config_version` is newer than the
firmware's persisted version. Equal and stale responses leave local state
unchanged. Local Serial overrides do not increment the backend version.

Firmware events are RAM-only, so at-least-once delivery applies only while the
current boot remains powered and the event remains buffered. Configuration is
persisted separately; actual output, sensor data, boot identity, event queue,
and ACK state are not persisted.
