CREATE TABLE device_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    boot_id INTEGER NOT NULL,
    seq INTEGER NOT NULL CHECK (seq > 0),
    event_type TEXT NOT NULL CHECK (
        event_type IN ('mode_changed', 'light_changed', 'wifi_changed')
    ),
    uptime_ms INTEGER NOT NULL CHECK (uptime_ms >= 0),
    mode TEXT NULL CHECK (mode IS NULL OR mode IN ('manual', 'night')),
    light_on INTEGER NULL CHECK (light_on IS NULL OR light_on IN (0, 1)),
    wifi_connected INTEGER NULL CHECK (
        wifi_connected IS NULL OR wifi_connected IN (0, 1)
    ),
    received_at TEXT NOT NULL,
    UNIQUE(device_id, boot_id, seq)
);

CREATE INDEX idx_device_events_recent
    ON device_events(received_at DESC, id DESC);
