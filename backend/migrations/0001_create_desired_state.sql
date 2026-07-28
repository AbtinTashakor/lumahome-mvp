CREATE TABLE desired_state (
    singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
    mode TEXT NOT NULL CHECK (mode IN ('manual', 'night')),
    manual_light_on INTEGER NOT NULL CHECK (manual_light_on IN (0, 1)),
    config_version INTEGER NOT NULL CHECK (config_version >= 1),
    updated_at TEXT NOT NULL
);
