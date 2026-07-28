use crate::persistence::{Persistence, PersistenceError};
use serde::{Deserialize, Serialize};
use std::{
    sync::{Arc, RwLock},
    time::{Duration, Instant},
};
use tokio::sync::Mutex;

pub const DEFAULT_DEVICE_ID: &str = "lumahome-01";
pub const DEVICE_ONLINE_TIMEOUT: Duration = Duration::from_secs(6);

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum HomeMode {
    Manual,
    Night,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum DeviceEvent {
    #[serde(rename = "mode_changed")]
    Mode {
        seq: u64,
        uptime_ms: u64,
        mode: HomeMode,
    },
    #[serde(rename = "light_changed")]
    Light {
        seq: u64,
        uptime_ms: u64,
        light_on: bool,
    },
    #[serde(rename = "wifi_changed")]
    Wifi {
        seq: u64,
        uptime_ms: u64,
        connected: bool,
    },
}

impl DeviceEvent {
    pub const fn seq(&self) -> u64 {
        match self {
            Self::Mode { seq, .. } | Self::Light { seq, .. } | Self::Wifi { seq, .. } => *seq,
        }
    }

    pub const fn uptime_ms(&self) -> u64 {
        match self {
            Self::Mode { uptime_ms, .. }
            | Self::Light { uptime_ms, .. }
            | Self::Wifi { uptime_ms, .. } => *uptime_ms,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct StoredDeviceEvent {
    pub device_id: String,
    pub boot_id: u64,
    #[serde(flatten)]
    pub event: DeviceEvent,
    pub received_at: String,
}

impl HomeMode {
    pub const fn as_db_text(self) -> &'static str {
        match self {
            Self::Manual => "manual",
            Self::Night => "night",
        }
    }

    pub fn from_db_text(value: &str) -> Option<Self> {
        match value {
            "manual" => Some(Self::Manual),
            "night" => Some(Self::Night),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct DesiredState {
    pub mode: HomeMode,
    pub manual_light_on: bool,
    pub config_version: u64,
}

impl Default for DesiredState {
    fn default() -> Self {
        Self {
            mode: HomeMode::Manual,
            manual_light_on: false,
            config_version: 1,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ReportedState {
    pub device_id: String,
    pub boot_id: u64,
    pub mode: HomeMode,
    pub light_on: bool,
    pub sensor_raw: u16,
    pub config_version: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct DashboardState {
    pub online: bool,
    pub desired: DesiredState,
    pub reported: Option<ReportedState>,
    pub last_seen: Option<String>,
}

#[derive(Clone)]
struct StoredState {
    desired: DesiredState,
    reported: Option<ReportedState>,
    last_seen: Option<String>,
    last_seen_instant: Option<Instant>,
}

impl StoredState {
    fn new(desired: DesiredState) -> Self {
        Self {
            desired,
            reported: None,
            last_seen: None,
            last_seen_instant: None,
        }
    }
}

#[derive(Clone)]
pub struct AppState {
    inner: Arc<RwLock<StoredState>>,
    persistence: Option<Persistence>,
    desired_mutation: Arc<Mutex<()>>,
    expected_device_id: Arc<str>,
    online_timeout: Duration,
}

impl Default for AppState {
    fn default() -> Self {
        Self::runtime_only(DEFAULT_DEVICE_ID.to_string())
    }
}

impl AppState {
    pub fn new(
        expected_device_id: String,
        desired: DesiredState,
        persistence: Persistence,
    ) -> Self {
        Self {
            inner: Arc::new(RwLock::new(StoredState::new(desired))),
            persistence: Some(persistence),
            desired_mutation: Arc::new(Mutex::new(())),
            expected_device_id: expected_device_id.into(),
            online_timeout: DEVICE_ONLINE_TIMEOUT,
        }
    }

    fn runtime_only(expected_device_id: String) -> Self {
        Self {
            inner: Arc::new(RwLock::new(StoredState::new(DesiredState::default()))),
            persistence: None,
            desired_mutation: Arc::new(Mutex::new(())),
            expected_device_id: expected_device_id.into(),
            online_timeout: DEVICE_ONLINE_TIMEOUT,
        }
    }

    pub fn expected_device_id(&self) -> &str {
        &self.expected_device_id
    }

    pub fn snapshot(&self) -> DashboardState {
        self.snapshot_at(Instant::now())
    }

    pub async fn consistent_snapshot(&self) -> DashboardState {
        let _mutation = self.desired_mutation.lock().await;
        self.snapshot()
    }

    pub fn snapshot_at(&self, now: Instant) -> DashboardState {
        let state = self.inner.read().expect("application state lock poisoned");
        let online = state.last_seen_instant.is_some_and(|last_seen| {
            now.saturating_duration_since(last_seen) <= self.online_timeout
        });

        DashboardState {
            online,
            desired: state.desired.clone(),
            reported: state.reported.clone(),
            last_seen: state.last_seen.clone(),
        }
    }

    // This mutex serializes desired-state reads, persistence, and in-memory replacement.
    pub async fn set_mode(&self, mode: HomeMode) -> Result<DashboardState, MutationError> {
        let _mutation = self.desired_mutation.lock().await;
        let current = self.current_desired();
        if current.mode == mode {
            return Ok(self.snapshot());
        }

        let next_desired = DesiredState {
            mode,
            manual_light_on: current.manual_light_on,
            config_version: next_version(current.config_version)?,
        };
        self.persist_and_replace(next_desired).await
    }

    pub async fn set_manual_light(&self, on: bool) -> Result<DashboardState, MutationError> {
        let _mutation = self.desired_mutation.lock().await;
        let current = self.current_desired();
        if current.mode != HomeMode::Manual {
            return Err(MutationError::ManualControlUnavailable);
        }
        if current.manual_light_on == on {
            return Ok(self.snapshot());
        }

        let next_desired = DesiredState {
            mode: current.mode,
            manual_light_on: on,
            config_version: next_version(current.config_version)?,
        };
        self.persist_and_replace(next_desired).await
    }

    pub async fn record_heartbeat(
        &self,
        reported: ReportedState,
        last_seen: String,
        events: &[DeviceEvent],
    ) -> Result<DesiredState, PersistenceError> {
        if let Some(persistence) = &self.persistence {
            persistence
                .save_device_events(&reported.device_id, reported.boot_id, events, &last_seen)
                .await?;
        }
        self.record_heartbeat_at(reported, last_seen, Instant::now());
        let _mutation = self.desired_mutation.lock().await;
        Ok(self.current_desired())
    }

    pub async fn recent_events(
        &self,
        limit: u32,
    ) -> Result<Vec<StoredDeviceEvent>, PersistenceError> {
        match &self.persistence {
            Some(persistence) => persistence.load_recent_events(limit).await,
            None => Ok(Vec::new()),
        }
    }

    pub fn record_heartbeat_at(&self, reported: ReportedState, last_seen: String, now: Instant) {
        let mut state = self.inner.write().expect("application state lock poisoned");
        state.reported = Some(reported);
        state.last_seen = Some(last_seen);
        state.last_seen_instant = Some(now);
    }

    fn current_desired(&self) -> DesiredState {
        self.inner
            .read()
            .expect("application state lock poisoned")
            .desired
            .clone()
    }

    async fn persist_and_replace(
        &self,
        next_desired: DesiredState,
    ) -> Result<DashboardState, MutationError> {
        let persistence = self
            .persistence
            .as_ref()
            .expect("persisted application state must have a database");
        persistence.save_desired_state(&next_desired).await?;
        let mut state = self.inner.write().expect("application state lock poisoned");
        state.desired = next_desired;
        Ok(snapshot_from_stored(
            &state,
            Instant::now(),
            self.online_timeout,
        ))
    }
}

#[derive(Debug)]
pub enum MutationError {
    ManualControlUnavailable,
    Persistence,
}

impl From<PersistenceError> for MutationError {
    fn from(_: PersistenceError) -> Self {
        Self::Persistence
    }
}

fn next_version(current: u64) -> Result<u64, MutationError> {
    current.checked_add(1).ok_or(MutationError::Persistence)
}

fn snapshot_from_stored(
    state: &StoredState,
    now: Instant,
    online_timeout: Duration,
) -> DashboardState {
    let online = state
        .last_seen_instant
        .is_some_and(|last_seen| now.saturating_duration_since(last_seen) <= online_timeout);

    DashboardState {
        online,
        desired: state.desired.clone(),
        reported: state.reported.clone(),
        last_seen: state.last_seen.clone(),
    }
}
