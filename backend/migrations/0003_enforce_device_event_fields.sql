CREATE TRIGGER device_events_validate_insert
BEFORE INSERT ON device_events
WHEN NEW.device_id = ''
    OR NEW.boot_id < 0
    OR NOT (
        (
            NEW.event_type = 'mode_changed'
            AND NEW.mode IS NOT NULL
            AND NEW.light_on IS NULL
            AND NEW.wifi_connected IS NULL
        )
        OR (
            NEW.event_type = 'light_changed'
            AND NEW.mode IS NULL
            AND NEW.light_on IS NOT NULL
            AND NEW.wifi_connected IS NULL
        )
        OR (
            NEW.event_type = 'wifi_changed'
            AND NEW.mode IS NULL
            AND NEW.light_on IS NULL
            AND NEW.wifi_connected IS NOT NULL
        )
    )
BEGIN
    SELECT RAISE(ABORT, 'invalid device event fields');
END;

CREATE TRIGGER device_events_validate_update
BEFORE UPDATE ON device_events
WHEN NEW.device_id = ''
    OR NEW.boot_id < 0
    OR NOT (
        (
            NEW.event_type = 'mode_changed'
            AND NEW.mode IS NOT NULL
            AND NEW.light_on IS NULL
            AND NEW.wifi_connected IS NULL
        )
        OR (
            NEW.event_type = 'light_changed'
            AND NEW.mode IS NULL
            AND NEW.light_on IS NOT NULL
            AND NEW.wifi_connected IS NULL
        )
        OR (
            NEW.event_type = 'wifi_changed'
            AND NEW.mode IS NULL
            AND NEW.light_on IS NULL
            AND NEW.wifi_connected IS NOT NULL
        )
    )
BEGIN
    SELECT RAISE(ABORT, 'invalid device event fields');
END;

UPDATE device_events SET event_type = event_type;
