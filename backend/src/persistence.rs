use crate::state::{DesiredState, DeviceEvent, HomeMode, StoredDeviceEvent};
use chrono::{SecondsFormat, Utc};
use sqlx::{
    sqlite::{SqliteConnectOptions, SqlitePoolOptions, SqliteSynchronous},
    SqlitePool,
};
use std::{fmt, path::Path, str::FromStr};

type DeviceEventRow = (
    String,
    i64,
    i64,
    String,
    i64,
    Option<String>,
    Option<i64>,
    Option<i64>,
    String,
);

static MIGRATOR: sqlx::migrate::Migrator = sqlx::migrate!();

#[derive(Clone)]
pub struct Persistence {
    pool: SqlitePool,
}

#[derive(Debug)]
pub enum PersistenceError {
    Database(sqlx::Error),
    Migration(sqlx::migrate::MigrateError),
    InvalidStoredState(String),
    InvalidDatabaseUrl(String),
}

impl fmt::Display for PersistenceError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Database(error) => write!(formatter, "database error: {error}"),
            Self::Migration(error) => write!(formatter, "migration error: {error}"),
            Self::InvalidStoredState(message) => {
                write!(formatter, "invalid persisted state: {message}")
            }
            Self::InvalidDatabaseUrl(message) => {
                write!(formatter, "invalid database URL: {message}")
            }
        }
    }
}

impl std::error::Error for PersistenceError {}

impl From<sqlx::Error> for PersistenceError {
    fn from(error: sqlx::Error) -> Self {
        Self::Database(error)
    }
}

impl From<sqlx::migrate::MigrateError> for PersistenceError {
    fn from(error: sqlx::migrate::MigrateError) -> Self {
        Self::Migration(error)
    }
}

impl Persistence {
    pub async fn initialize(database_url: &str) -> Result<(Self, DesiredState), PersistenceError> {
        ensure_database_parent(database_url)?;
        let options = SqliteConnectOptions::from_str(database_url)
            .map_err(|error| PersistenceError::InvalidDatabaseUrl(error.to_string()))?
            .create_if_missing(true)
            .synchronous(SqliteSynchronous::Full);
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect_with(options)
            .await?;

        MIGRATOR.run(&pool).await?;
        sqlx::query(
            "INSERT INTO desired_state (singleton_id, mode, manual_light_on, config_version, updated_at) \
             VALUES (1, 'manual', 0, 1, ?) ON CONFLICT(singleton_id) DO NOTHING",
        )
        .bind(timestamp())
        .execute(&pool)
        .await?;

        let persistence = Self { pool };
        let desired = persistence.load_desired_state().await?;
        Ok((persistence, desired))
    }

    pub async fn save_desired_state(&self, desired: &DesiredState) -> Result<(), PersistenceError> {
        let version = i64::try_from(desired.config_version).map_err(|_| {
            PersistenceError::InvalidStoredState(
                "config_version exceeds SQLite integer range".to_string(),
            )
        })?;
        let result = sqlx::query(
            "UPDATE desired_state SET mode = ?, manual_light_on = ?, config_version = ?, updated_at = ? \
             WHERE singleton_id = 1",
        )
        .bind(desired.mode.as_db_text())
        .bind(i64::from(desired.manual_light_on))
        .bind(version)
        .bind(timestamp())
        .execute(&self.pool)
        .await?;

        if result.rows_affected() != 1 {
            return Err(PersistenceError::InvalidStoredState(
                "desired-state singleton row is missing".to_string(),
            ));
        }
        Ok(())
    }

    pub async fn save_device_events(
        &self,
        device_id: &str,
        boot_id: u64,
        events: &[DeviceEvent],
        received_at: &str,
    ) -> Result<(), PersistenceError> {
        if events.is_empty() {
            return Ok(());
        }

        let boot_id = sqlite_integer(boot_id, "boot_id")?;
        let mut transaction = self.pool.begin().await?;

        for event in events {
            let (event_type, mode, light_on, wifi_connected) = match event {
                DeviceEvent::Mode { mode, .. } => {
                    ("mode_changed", Some(mode.as_db_text()), None, None)
                }
                DeviceEvent::Light { light_on, .. } => {
                    ("light_changed", None, Some(i64::from(*light_on)), None)
                }
                DeviceEvent::Wifi { connected, .. } => {
                    ("wifi_changed", None, None, Some(i64::from(*connected)))
                }
            };

            sqlx::query(
                "INSERT INTO device_events \
                 (device_id, boot_id, seq, event_type, uptime_ms, mode, light_on, wifi_connected, received_at) \
                 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) \
                 ON CONFLICT(device_id, boot_id, seq) DO NOTHING",
            )
            .bind(device_id)
            .bind(boot_id)
            .bind(sqlite_integer(event.seq(), "event seq")?)
            .bind(event_type)
            .bind(sqlite_integer(event.uptime_ms(), "event uptime_ms")?)
            .bind(mode)
            .bind(light_on)
            .bind(wifi_connected)
            .bind(received_at)
            .execute(&mut *transaction)
            .await?;
        }

        transaction.commit().await?;
        Ok(())
    }

    pub async fn load_recent_events(
        &self,
        limit: u32,
    ) -> Result<Vec<StoredDeviceEvent>, PersistenceError> {
        let rows = sqlx::query_as::<_, DeviceEventRow>(
            "SELECT device_id, boot_id, seq, event_type, uptime_ms, mode, light_on, wifi_connected, received_at \
             FROM device_events ORDER BY received_at DESC, id DESC LIMIT ?",
        )
        .bind(i64::from(limit))
        .fetch_all(&self.pool)
        .await?;

        rows.into_iter().map(stored_event_from_row).collect()
    }

    async fn load_desired_state(&self) -> Result<DesiredState, PersistenceError> {
        let row = sqlx::query_as::<_, (String, i64, i64)>(
            "SELECT mode, manual_light_on, config_version FROM desired_state WHERE singleton_id = 1",
        )
        .fetch_optional(&self.pool)
        .await?
        .ok_or_else(|| PersistenceError::InvalidStoredState("desired-state singleton row is missing".to_string()))?;

        let mode = HomeMode::from_db_text(&row.0).ok_or_else(|| {
            PersistenceError::InvalidStoredState(format!("unsupported mode '{}'", row.0))
        })?;
        let manual_light_on = match row.1 {
            0 => false,
            1 => true,
            value => {
                return Err(PersistenceError::InvalidStoredState(format!(
                    "manual_light_on must be 0 or 1, got {value}"
                )));
            }
        };
        let config_version = u64::try_from(row.2).map_err(|_| {
            PersistenceError::InvalidStoredState(format!(
                "config_version must be positive, got {}",
                row.2
            ))
        })?;
        if config_version == 0 {
            return Err(PersistenceError::InvalidStoredState(
                "config_version must be at least 1".to_string(),
            ));
        }

        Ok(DesiredState {
            mode,
            manual_light_on,
            config_version,
        })
    }
}

fn stored_event_from_row(row: DeviceEventRow) -> Result<StoredDeviceEvent, PersistenceError> {
    let (device_id, boot_id, seq, event_type, uptime_ms, mode, light_on, wifi, received_at) = row;
    let boot_id = stored_unsigned(boot_id, "boot_id")?;
    let seq = stored_unsigned(seq, "event seq")?;
    let uptime_ms = stored_unsigned(uptime_ms, "event uptime_ms")?;
    let event = match event_type.as_str() {
        "mode_changed" => DeviceEvent::Mode {
            seq,
            uptime_ms,
            mode: mode
                .as_deref()
                .and_then(HomeMode::from_db_text)
                .ok_or_else(|| invalid_event("mode_changed event has invalid mode"))?,
        },
        "light_changed" => DeviceEvent::Light {
            seq,
            uptime_ms,
            light_on: stored_boolean(light_on, "light_changed event light_on")?,
        },
        "wifi_changed" => DeviceEvent::Wifi {
            seq,
            uptime_ms,
            connected: stored_boolean(wifi, "wifi_changed event wifi_connected")?,
        },
        _ => return Err(invalid_event("event_type is unsupported")),
    };

    Ok(StoredDeviceEvent {
        device_id,
        boot_id,
        event,
        received_at,
    })
}

fn sqlite_integer(value: u64, field: &str) -> Result<i64, PersistenceError> {
    i64::try_from(value)
        .map_err(|_| invalid_event(&format!("{field} exceeds SQLite integer range")))
}

fn stored_unsigned(value: i64, field: &str) -> Result<u64, PersistenceError> {
    u64::try_from(value).map_err(|_| invalid_event(&format!("{field} must not be negative")))
}

fn stored_boolean(value: Option<i64>, field: &str) -> Result<bool, PersistenceError> {
    match value {
        Some(0) => Ok(false),
        Some(1) => Ok(true),
        _ => Err(invalid_event(&format!("{field} must be 0 or 1"))),
    }
}

fn invalid_event(message: &str) -> PersistenceError {
    PersistenceError::InvalidStoredState(message.to_string())
}

fn timestamp() -> String {
    Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true)
}

fn ensure_database_parent(database_url: &str) -> Result<(), PersistenceError> {
    let Some(path) = database_url.strip_prefix("sqlite://") else {
        return Ok(());
    };
    let path = path.split_once('?').map_or(path, |(path, _)| path);
    if path == ":memory:" || path.starts_with("file:") {
        return Ok(());
    }
    let path = Path::new(path);
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent).map_err(|error| {
            PersistenceError::InvalidDatabaseUrl(format!(
                "could not create database directory: {error}"
            ))
        })?;
    }
    Ok(())
}
