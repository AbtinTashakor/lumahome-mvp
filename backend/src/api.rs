use crate::{
    error::ApiError,
    persistence::{Persistence, PersistenceError},
    state::{
        AppState, DashboardState, DesiredState, DeviceEvent, HomeMode, MutationError,
        ReportedState, StoredDeviceEvent, DEFAULT_DEVICE_ID,
    },
};
use axum::{
    extract::{
        rejection::{JsonRejection, QueryRejection},
        Query, State,
    },
    routing::{any, get, post},
    Json, Router,
};
use chrono::{SecondsFormat, Utc};
use serde::{Deserialize, Serialize};
use std::{env, path::PathBuf};
use tower_http::services::ServeDir;

pub const HEARTBEAT_INTERVAL_MS: u64 = 2_000;
pub const MAX_EVENTS_PER_HEARTBEAT: usize = 32;
pub const DEFAULT_EVENT_LIMIT: u32 = 10;
pub const MAX_EVENT_LIMIT: i64 = 50;

#[derive(Serialize)]
struct HealthResponse {
    status: &'static str,
    service: &'static str,
}

#[derive(Deserialize)]
struct ModeRequest {
    mode: String,
}

#[derive(Deserialize)]
struct LightRequest {
    on: bool,
}

#[derive(Deserialize)]
struct HeartbeatRequest {
    device_id: String,
    boot_id: u64,
    mode: String,
    light_on: bool,
    sensor_raw: u16,
    config_version: u64,
    #[serde(default)]
    events: Vec<DeviceEvent>,
}

#[derive(Serialize)]
struct HeartbeatResponse {
    desired: DesiredState,
    heartbeat_interval_ms: u64,
    events_ack_seq: Option<u64>,
}

#[derive(Deserialize)]
struct EventsQuery {
    limit: Option<i64>,
}

#[derive(Serialize)]
struct EventsResponse {
    events: Vec<StoredDeviceEvent>,
}

pub async fn app() -> Result<Router, PersistenceError> {
    let expected_device_id = env::var("LUMAHOME_DEVICE_ID")
        .ok()
        .filter(|device_id| !device_id.is_empty())
        .unwrap_or_else(|| DEFAULT_DEVICE_ID.to_string());
    app_with_database_url(expected_device_id, &database_url()).await
}

async fn app_with_database_url(
    expected_device_id: String,
    database_url: &str,
) -> Result<Router, PersistenceError> {
    let (persistence, desired) = Persistence::initialize(database_url).await?;
    Ok(app_with_state(AppState::new(
        expected_device_id,
        desired,
        persistence,
    )))
}

fn database_url() -> String {
    env::var("LUMAHOME_DATABASE_URL").unwrap_or_else(|_| {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("data")
            .join("lumahome.db");
        format!("sqlite://{}", path.to_string_lossy().replace('\\', "/"))
    })
}

fn app_with_state(state: AppState) -> Router {
    let static_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("static");

    Router::new()
        .route("/api/health", get(health).fallback(method_not_allowed))
        .route("/api/state", get(get_state).fallback(method_not_allowed))
        .route("/api/events", get(get_events).fallback(method_not_allowed))
        .route("/api/mode", post(set_mode).fallback(method_not_allowed))
        .route("/api/light", post(set_light).fallback(method_not_allowed))
        .route(
            "/api/device/heartbeat",
            post(device_heartbeat).fallback(method_not_allowed),
        )
        .route("/api", any(api_not_found))
        .route("/api/{*path}", any(api_not_found))
        .fallback_service(ServeDir::new(static_dir).append_index_html_on_directories(true))
        .with_state(state)
}

async fn health() -> Json<HealthResponse> {
    Json(HealthResponse {
        status: "ok",
        service: "lumahome-backend",
    })
}

async fn get_state(State(state): State<AppState>) -> Json<DashboardState> {
    Json(state.consistent_snapshot().await)
}

async fn get_events(
    State(state): State<AppState>,
    query: Result<Query<EventsQuery>, QueryRejection>,
) -> Result<Json<EventsResponse>, ApiError> {
    let Query(query) = query.map_err(|_| ApiError::invalid_event_limit())?;
    let limit = match query.limit {
        None => DEFAULT_EVENT_LIMIT,
        Some(limit) if (1..=MAX_EVENT_LIMIT).contains(&limit) => limit as u32,
        Some(_) => return Err(ApiError::invalid_event_limit()),
    };
    let events = state
        .recent_events(limit)
        .await
        .map_err(|_| ApiError::persistence_failed())?;
    Ok(Json(EventsResponse { events }))
}

async fn set_mode(
    State(state): State<AppState>,
    payload: Result<Json<ModeRequest>, JsonRejection>,
) -> Result<Json<DashboardState>, ApiError> {
    let Json(request) = payload.map_err(|_| ApiError::invalid_json())?;
    let mode = match request.mode.as_str() {
        "manual" => HomeMode::Manual,
        "night" => HomeMode::Night,
        _ => return Err(ApiError::unsupported_mode()),
    };

    state
        .set_mode(mode)
        .await
        .map(Json)
        .map_err(map_mutation_error)
}

async fn set_light(
    State(state): State<AppState>,
    payload: Result<Json<LightRequest>, JsonRejection>,
) -> Result<Json<DashboardState>, ApiError> {
    let Json(request) = payload.map_err(|_| ApiError::invalid_json())?;
    state
        .set_manual_light(request.on)
        .await
        .map(Json)
        .map_err(map_mutation_error)
}

async fn device_heartbeat(
    State(state): State<AppState>,
    payload: Result<Json<HeartbeatRequest>, JsonRejection>,
) -> Result<Json<HeartbeatResponse>, ApiError> {
    let Json(request) = payload.map_err(|_| ApiError::invalid_json())?;

    if request.device_id.is_empty() || request.device_id != state.expected_device_id() {
        return Err(ApiError::device_not_allowed());
    }

    let mode = match request.mode.as_str() {
        "manual" => HomeMode::Manual,
        "night" => HomeMode::Night,
        _ => return Err(ApiError::unsupported_mode()),
    };

    validate_event_batch(request.boot_id, &request.events)?;
    let events_ack_seq = request.events.last().map(DeviceEvent::seq);

    let reported = ReportedState {
        device_id: request.device_id,
        boot_id: request.boot_id,
        mode,
        light_on: request.light_on,
        sensor_raw: request.sensor_raw,
        config_version: request.config_version,
    };
    let last_seen = Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true);
    let desired = state
        .record_heartbeat(reported, last_seen, &request.events)
        .await
        .map_err(|_| ApiError::persistence_failed())?;

    Ok(Json(HeartbeatResponse {
        desired,
        heartbeat_interval_ms: HEARTBEAT_INTERVAL_MS,
        events_ack_seq,
    }))
}

fn validate_event_batch(boot_id: u64, events: &[DeviceEvent]) -> Result<(), ApiError> {
    if events.len() > MAX_EVENTS_PER_HEARTBEAT {
        return Err(ApiError::event_batch_too_large());
    }
    if events.is_empty() {
        return Ok(());
    }
    if i64::try_from(boot_id).is_err() {
        return Err(ApiError::invalid_event_batch());
    }

    let mut previous_seq = 0;
    for event in events {
        if event.seq() == 0
            || event.seq() <= previous_seq
            || i64::try_from(event.seq()).is_err()
            || i64::try_from(event.uptime_ms()).is_err()
        {
            return Err(ApiError::invalid_event_batch());
        }
        previous_seq = event.seq();
    }
    Ok(())
}

async fn api_not_found() -> ApiError {
    ApiError::not_found()
}

async fn method_not_allowed() -> ApiError {
    ApiError::method_not_allowed()
}

fn map_mutation_error(error: MutationError) -> ApiError {
    match error {
        MutationError::ManualControlUnavailable => ApiError::manual_control_unavailable(),
        MutationError::Persistence => ApiError::persistence_failed(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::state::DEVICE_ONLINE_TIMEOUT;
    use axum::{
        body::{to_bytes, Body},
        http::{header::CONTENT_TYPE, Method, Request, StatusCode},
    };
    use serde_json::{json, Value};
    use sqlx::sqlite::SqlitePoolOptions;
    use std::{
        path::Path,
        time::{Duration, Instant},
    };
    use tempfile::TempDir;
    use tower::ServiceExt;

    const HEARTBEAT: &str = r#"{
        "device_id":"lumahome-01",
        "boot_id":4182,
        "mode":"night",
        "light_on":true,
        "sensor_raw":280,
        "config_version":1
    }"#;

    async fn test_app() -> (Router, TempDir) {
        let directory = tempfile::tempdir().unwrap();
        let database_path = directory.path().join("lumahome-test.db");
        let database_url = sqlite_url(&database_path);
        let app = app_with_database_url(DEFAULT_DEVICE_ID.to_string(), &database_url)
            .await
            .unwrap();
        (app, directory)
    }

    async fn test_app_at(database_url: &str) -> Router {
        app_with_database_url(DEFAULT_DEVICE_ID.to_string(), database_url)
            .await
            .unwrap()
    }

    fn sqlite_url(path: &Path) -> String {
        format!("sqlite://{}", path.to_string_lossy().replace('\\', "/"))
    }

    async fn request(method: Method, uri: &str, body: Option<&str>) -> (StatusCode, Value) {
        let mut builder = Request::builder().method(method).uri(uri);
        let request_body = if let Some(body) = body {
            builder = builder.header(CONTENT_TYPE, "application/json");
            Body::from(body.to_owned())
        } else {
            Body::empty()
        };
        let (app, _database) = test_app().await;
        let response = app
            .oneshot(builder.body(request_body).unwrap())
            .await
            .unwrap();
        let status = response.status();
        let bytes = to_bytes(response.into_body(), usize::MAX).await.unwrap();

        (status, serde_json::from_slice(&bytes).unwrap())
    }

    fn heartbeat_body(boot_id: u64, events: Value) -> Value {
        json!({
            "device_id": DEFAULT_DEVICE_ID,
            "boot_id": boot_id,
            "mode": "night",
            "light_on": true,
            "sensor_raw": 280,
            "config_version": 1,
            "events": events
        })
    }

    async fn send_heartbeat(app: &Router, body: Value) -> (StatusCode, Value) {
        let response = app
            .clone()
            .oneshot(json_request("/api/device/heartbeat", body.to_string()))
            .await
            .unwrap();
        let status = response.status();
        (status, response_json(response).await)
    }

    async fn get_json(app: &Router, uri: &str) -> (StatusCode, Value) {
        let response = app
            .clone()
            .oneshot(Request::builder().uri(uri).body(Body::empty()).unwrap())
            .await
            .unwrap();
        let status = response.status();
        (status, response_json(response).await)
    }

    #[tokio::test]
    async fn initial_state_is_offline_with_no_report() {
        let (status, body) = request(Method::GET, "/api/state", None).await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(
            body,
            json!({
                "online": false,
                "desired": {
                    "mode": "manual",
                    "manual_light_on": false,
                    "config_version": 1
                },
                "reported": null,
                "last_seen": null
            })
        );
    }

    #[tokio::test]
    async fn valid_heartbeat_returns_latest_desired_state() {
        let (app, _database) = test_app().await;
        app.clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#))
            .await
            .unwrap();
        let response = app
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        let status = response.status();
        let body = response_json(response).await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(body["desired"]["mode"], "night");
        assert_eq!(body["desired"]["manual_light_on"], false);
        assert_eq!(body["desired"]["config_version"], 2);
        assert_eq!(body["heartbeat_interval_ms"], HEARTBEAT_INTERVAL_MS);
        assert_eq!(body["events_ack_seq"], Value::Null);
    }

    #[tokio::test]
    async fn empty_event_list_is_valid_and_has_no_acknowledgement() {
        let (app, _database) = test_app().await;
        let (status, body) = send_heartbeat(&app, heartbeat_body(4182, json!([]))).await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(body["events_ack_seq"], Value::Null);
        let (_, events) = get_json(&app, "/api/events").await;
        assert_eq!(events, json!({ "events": [] }));
    }

    #[tokio::test]
    async fn valid_single_event_is_stored_with_only_its_variant_field() {
        let (app, _database) = test_app().await;
        let event = json!({
            "seq": 21,
            "uptime_ms": 8100,
            "type": "mode_changed",
            "mode": "night"
        });
        let (status, heartbeat) = send_heartbeat(&app, heartbeat_body(4182, json!([event]))).await;
        let (_, body) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(heartbeat["events_ack_seq"], 21);
        assert_eq!(body["events"].as_array().unwrap().len(), 1);
        assert_eq!(body["events"][0]["device_id"], DEFAULT_DEVICE_ID);
        assert_eq!(body["events"][0]["boot_id"], 4182);
        assert_eq!(body["events"][0]["seq"], 21);
        assert_eq!(body["events"][0]["uptime_ms"], 8100);
        assert_eq!(body["events"][0]["type"], "mode_changed");
        assert_eq!(body["events"][0]["mode"], "night");
        assert!(body["events"][0].get("light_on").is_none());
        assert!(body["events"][0].get("connected").is_none());
        assert!(body["events"][0]["received_at"]
            .as_str()
            .unwrap()
            .ends_with('Z'));
    }

    #[tokio::test]
    async fn migration_rejects_invalid_event_column_combinations() {
        let directory = tempfile::tempdir().unwrap();
        let database_url = sqlite_url(&directory.path().join("event-integrity.db"));
        let app = test_app_at(&database_url).await;
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect(&database_url)
            .await
            .unwrap();

        let invalid_insert = sqlx::query(
            "INSERT INTO device_events \
             (device_id, boot_id, seq, event_type, uptime_ms, mode, light_on, wifi_connected, received_at) \
             VALUES ('lumahome-01', 1, 1, 'mode_changed', 0, NULL, 1, NULL, '2026-07-22T10:30:00.000Z')",
        )
        .execute(&pool)
        .await;
        assert!(invalid_insert.is_err());

        send_heartbeat(
            &app,
            heartbeat_body(
                1,
                json!([{"seq": 1, "uptime_ms": 0, "type": "light_changed", "light_on": true}]),
            ),
        )
        .await;
        let invalid_update = sqlx::query(
            "UPDATE device_events SET mode = 'night' \
             WHERE device_id = 'lumahome-01' AND boot_id = 1 AND seq = 1",
        )
        .execute(&pool)
        .await;
        assert!(invalid_update.is_err());
    }

    #[tokio::test]
    async fn valid_multi_event_batch_is_stored_and_highest_sequence_is_acknowledged() {
        let (app, _database) = test_app().await;
        let events = json!([
            {"seq": 21, "uptime_ms": 8100, "type": "mode_changed", "mode": "night"},
            {"seq": 22, "uptime_ms": 8400, "type": "light_changed", "light_on": true},
            {"seq": 23, "uptime_ms": 12000, "type": "wifi_changed", "connected": false}
        ]);
        let (status, heartbeat) = send_heartbeat(&app, heartbeat_body(4182, events)).await;
        let (_, body) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(heartbeat["events_ack_seq"], 23);
        assert_eq!(body["events"].as_array().unwrap().len(), 3);
    }

    #[tokio::test]
    async fn duplicate_single_event_retry_is_acknowledged_without_another_row() {
        let (app, _database) = test_app().await;
        let body = heartbeat_body(
            4182,
            json!([{"seq": 1, "uptime_ms": 100, "type": "light_changed", "light_on": true}]),
        );
        send_heartbeat(&app, body.clone()).await;
        let (status, retry) = send_heartbeat(&app, body).await;
        let (_, events) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(retry["events_ack_seq"], 1);
        assert_eq!(events["events"].as_array().unwrap().len(), 1);
    }

    #[tokio::test]
    async fn duplicate_complete_batch_retry_remains_successful_without_duplicates() {
        let (app, _database) = test_app().await;
        let body = heartbeat_body(
            4182,
            json!([
                {"seq": 1, "uptime_ms": 100, "type": "light_changed", "light_on": true},
                {"seq": 2, "uptime_ms": 200, "type": "wifi_changed", "connected": false}
            ]),
        );
        send_heartbeat(&app, body.clone()).await;
        let (status, retry) = send_heartbeat(&app, body).await;
        let (_, events) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(retry["events_ack_seq"], 2);
        assert_eq!(events["events"].as_array().unwrap().len(), 2);
    }

    #[tokio::test]
    async fn same_sequence_under_another_boot_id_is_accepted() {
        let (app, _database) = test_app().await;
        let event = json!([
            {"seq": 1, "uptime_ms": 100, "type": "wifi_changed", "connected": true}
        ]);
        send_heartbeat(&app, heartbeat_body(1, event.clone())).await;
        let (status, _) = send_heartbeat(&app, heartbeat_body(2, event)).await;
        let (_, events) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(events["events"].as_array().unwrap().len(), 2);
    }

    #[tokio::test]
    async fn another_device_identity_cannot_store_an_event() {
        let (app, _database) = test_app().await;
        let mut body = heartbeat_body(
            1,
            json!([{"seq": 1, "uptime_ms": 0, "type": "wifi_changed", "connected": true}]),
        );
        body["device_id"] = json!("other-device");
        let (status, response) = send_heartbeat(&app, body).await;
        let (_, events) = get_json(&app, "/api/events").await;

        assert_eq!(status, StatusCode::FORBIDDEN);
        assert_eq!(response["error"]["code"], "device_not_allowed");
        assert_eq!(events, json!({"events": []}));
    }

    #[tokio::test]
    async fn events_are_returned_newest_first() {
        let (app, _database) = test_app().await;
        let events = json!([
            {"seq": 1, "uptime_ms": 100, "type": "light_changed", "light_on": false},
            {"seq": 2, "uptime_ms": 200, "type": "light_changed", "light_on": true},
            {"seq": 3, "uptime_ms": 300, "type": "wifi_changed", "connected": false}
        ]);
        send_heartbeat(&app, heartbeat_body(1, events)).await;
        let (_, body) = get_json(&app, "/api/events").await;
        let events = body["events"].as_array().unwrap();

        assert_eq!(events[0]["received_at"], events[1]["received_at"]);
        assert_eq!(events[1]["received_at"], events[2]["received_at"]);
        assert_eq!(events[0]["seq"], 3);
        assert_eq!(events[1]["seq"], 2);
        assert_eq!(events[2]["seq"], 1);
    }

    #[tokio::test]
    async fn event_api_defaults_to_ten_rows() {
        let (app, _database) = test_app().await;
        let events: Vec<Value> = (1..=12)
            .map(|seq| {
                json!({"seq": seq, "uptime_ms": seq * 100, "type": "wifi_changed", "connected": true})
            })
            .collect();
        send_heartbeat(&app, heartbeat_body(1, json!(events))).await;
        let (_, body) = get_json(&app, "/api/events").await;

        assert_eq!(body["events"].as_array().unwrap().len(), 10);
    }

    #[tokio::test]
    async fn invalid_event_api_limits_return_json_bad_request() {
        let (app, _database) = test_app().await;
        for uri in [
            "/api/events?limit=0",
            "/api/events?limit=51",
            "/api/events?limit=nope",
        ] {
            let (status, body) = get_json(&app, uri).await;
            assert_eq!(status, StatusCode::BAD_REQUEST);
            assert_eq!(body["error"]["code"], "invalid_event_limit");
        }
    }

    #[tokio::test]
    async fn event_batch_larger_than_32_is_rejected() {
        let (app, _database) = test_app().await;
        let events: Vec<Value> = (1..=33)
            .map(|seq| {
                json!({"seq": seq, "uptime_ms": 0, "type": "wifi_changed", "connected": true})
            })
            .collect();
        let (status, body) = send_heartbeat(&app, heartbeat_body(1, json!(events))).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "event_batch_too_large");
    }

    #[tokio::test]
    async fn decreasing_event_sequences_are_rejected() {
        let (app, _database) = test_app().await;
        let events = json!([
            {"seq": 2, "uptime_ms": 100, "type": "wifi_changed", "connected": true},
            {"seq": 1, "uptime_ms": 200, "type": "wifi_changed", "connected": false}
        ]);
        let (status, body) = send_heartbeat(&app, heartbeat_body(1, events)).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_event_batch");
    }

    #[tokio::test]
    async fn duplicate_sequences_inside_one_request_are_rejected() {
        let (app, _database) = test_app().await;
        let events = json!([
            {"seq": 1, "uptime_ms": 100, "type": "wifi_changed", "connected": true},
            {"seq": 1, "uptime_ms": 200, "type": "wifi_changed", "connected": false}
        ]);
        let (status, body) = send_heartbeat(&app, heartbeat_body(1, events)).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_event_batch");
    }

    #[tokio::test]
    async fn zero_event_sequence_is_rejected() {
        let (app, _database) = test_app().await;
        let events = json!([
            {"seq": 0, "uptime_ms": 0, "type": "wifi_changed", "connected": true}
        ]);
        let (status, body) = send_heartbeat(&app, heartbeat_body(1, events)).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_event_batch");
    }

    #[tokio::test]
    async fn unknown_event_type_returns_json_bad_request() {
        let (status, body) = request(
            Method::POST,
            "/api/device/heartbeat",
            Some(r#"{"device_id":"lumahome-01","boot_id":1,"mode":"night","light_on":true,"sensor_raw":1,"config_version":1,"events":[{"seq":1,"uptime_ms":0,"type":"temperature_changed","value":20}]}"#),
        )
        .await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_json");
    }

    #[tokio::test]
    async fn invalid_event_specific_field_returns_json_bad_request() {
        let (status, body) = request(
            Method::POST,
            "/api/device/heartbeat",
            Some(r#"{"device_id":"lumahome-01","boot_id":1,"mode":"night","light_on":true,"sensor_raw":1,"config_version":1,"events":[{"seq":1,"uptime_ms":0,"type":"light_changed","light_on":"yes"}]}"#),
        )
        .await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_json");
    }

    #[tokio::test]
    async fn failed_event_transaction_rolls_back_and_does_not_update_runtime_state() {
        let directory = tempfile::tempdir().unwrap();
        let database_url = sqlite_url(&directory.path().join("failed-events.db"));
        let (persistence, desired) = Persistence::initialize(&database_url).await.unwrap();
        let sabotage_pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect(&database_url)
            .await
            .unwrap();
        sqlx::query(
            "CREATE TRIGGER fail_second_event BEFORE INSERT ON device_events \
             WHEN NEW.seq = 2 BEGIN SELECT RAISE(FAIL, 'forced test failure'); END",
        )
        .execute(&sabotage_pool)
        .await
        .unwrap();
        let app = app_with_state(AppState::new(
            DEFAULT_DEVICE_ID.to_string(),
            desired,
            persistence,
        ));
        let events = json!([
            {"seq": 1, "uptime_ms": 100, "type": "wifi_changed", "connected": true},
            {"seq": 2, "uptime_ms": 200, "type": "wifi_changed", "connected": false}
        ]);
        let (status, body) = send_heartbeat(&app, heartbeat_body(1, events)).await;
        sqlx::query("DROP TRIGGER fail_second_event")
            .execute(&sabotage_pool)
            .await
            .unwrap();
        let (_, stored) = get_json(&app, "/api/events").await;
        let (_, state) = get_json(&app, "/api/state").await;

        assert_eq!(status, StatusCode::INTERNAL_SERVER_ERROR);
        assert_eq!(body["error"]["code"], "persistence_failed");
        assert_eq!(stored, json!({"events": []}));
        assert_eq!(state["online"], false);
        assert_eq!(state["reported"], Value::Null);
        assert_eq!(state["last_seen"], Value::Null);
    }

    #[tokio::test]
    async fn duplicate_retry_still_updates_current_report() {
        let (app, _database) = test_app().await;
        let event = json!([
            {"seq": 1, "uptime_ms": 100, "type": "light_changed", "light_on": true}
        ]);
        send_heartbeat(&app, heartbeat_body(1, event.clone())).await;
        let mut retry = heartbeat_body(1, event);
        retry["mode"] = json!("manual");
        retry["light_on"] = json!(false);
        retry["sensor_raw"] = json!(512);
        let (status, _) = send_heartbeat(&app, retry).await;
        let (_, state) = get_json(&app, "/api/state").await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(state["reported"]["mode"], "manual");
        assert_eq!(state["reported"]["light_on"], false);
        assert_eq!(state["reported"]["sensor_raw"], 512);
    }

    #[tokio::test]
    async fn restart_restores_events_while_runtime_report_resets() {
        let directory = tempfile::tempdir().unwrap();
        let database_url = sqlite_url(&directory.path().join("event-restart.db"));
        let first = test_app_at(&database_url).await;
        send_heartbeat(
            &first,
            heartbeat_body(
                1,
                json!([{"seq": 1, "uptime_ms": 0, "type": "wifi_changed", "connected": true}]),
            ),
        )
        .await;

        let restarted = test_app_at(&database_url).await;
        let (_, events) = get_json(&restarted, "/api/events").await;
        let (_, state) = get_json(&restarted, "/api/state").await;

        assert_eq!(events["events"].as_array().unwrap().len(), 1);
        assert_eq!(state["online"], false);
        assert_eq!(state["reported"], Value::Null);
        assert_eq!(state["last_seen"], Value::Null);
    }

    #[tokio::test]
    async fn valid_heartbeat_stores_report_and_sets_last_seen_and_online() {
        let (app, _database) = test_app().await;
        app.clone()
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        let response = app
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let body = response_json(response).await;

        assert_eq!(body["online"], true);
        assert_eq!(
            body["reported"],
            json!({
                "device_id": "lumahome-01",
                "boot_id": 4182,
                "mode": "night",
                "light_on": true,
                "sensor_raw": 280,
                "config_version": 1
            })
        );
        let last_seen = body["last_seen"].as_str().unwrap();
        assert!(last_seen.ends_with('Z'));
        assert!(chrono::DateTime::parse_from_rfc3339(last_seen).is_ok());
    }

    #[test]
    fn heartbeat_times_out_without_discarding_last_report() {
        let state = AppState::default();
        let received_at = Instant::now();
        let reported = ReportedState {
            device_id: DEFAULT_DEVICE_ID.to_string(),
            boot_id: 1,
            mode: HomeMode::Manual,
            light_on: false,
            sensor_raw: 42,
            config_version: 1,
        };
        state.record_heartbeat_at(
            reported.clone(),
            "2026-07-22T10:30:00Z".to_string(),
            received_at,
        );

        assert!(
            state
                .snapshot_at(received_at + Duration::from_secs(5))
                .online
        );
        assert!(
            state
                .snapshot_at(received_at + DEVICE_ONLINE_TIMEOUT)
                .online
        );
        let expired =
            state.snapshot_at(received_at + DEVICE_ONLINE_TIMEOUT + Duration::from_millis(1));
        assert!(!expired.online);
        assert_eq!(expired.reported, Some(reported));
        assert_eq!(expired.last_seen.as_deref(), Some("2026-07-22T10:30:00Z"));
    }

    #[tokio::test]
    async fn second_heartbeat_replaces_the_previous_report() {
        let (app, _database) = test_app().await;
        app.clone()
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        app.clone()
            .oneshot(json_request(
                "/api/device/heartbeat",
                r#"{
                    "device_id":"lumahome-01",
                    "boot_id":4183,
                    "mode":"manual",
                    "light_on":false,
                    "sensor_raw":512,
                    "config_version":2
                }"#,
            ))
            .await
            .unwrap();
        let response = app
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let body = response_json(response).await;

        assert_eq!(body["reported"]["boot_id"], 4183);
        assert_eq!(body["reported"]["mode"], "manual");
        assert_eq!(body["reported"]["light_on"], false);
        assert_eq!(body["reported"]["sensor_raw"], 512);
        assert_eq!(body["reported"]["config_version"], 2);
    }

    #[tokio::test]
    async fn heartbeat_never_overwrites_desired_state() {
        let (app, _database) = test_app().await;
        app.clone()
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();
        let response = app
            .clone()
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        let body = response_json(response).await;

        assert_eq!(body["desired"]["mode"], "manual");
        assert_eq!(body["desired"]["manual_light_on"], true);
        assert_eq!(body["desired"]["config_version"], 2);

        let response = app
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let body = response_json(response).await;
        assert_eq!(body["desired"]["manual_light_on"], true);
    }

    #[tokio::test]
    async fn same_device_version_still_receives_current_desired_state() {
        let (app, _database) = test_app().await;
        let response = app
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        let body = response_json(response).await;

        assert_eq!(body["desired"]["config_version"], 1);
        assert_eq!(body["desired"]["mode"], "manual");
    }

    #[tokio::test]
    async fn unexpected_device_id_returns_json_forbidden() {
        let (status, body) = request(
            Method::POST,
            "/api/device/heartbeat",
            Some(
                r#"{
                "device_id":"other-device",
                "boot_id":1,
                "mode":"manual",
                "light_on":false,
                "sensor_raw":0,
                "config_version":1
            }"#,
            ),
        )
        .await;

        assert_eq!(status, StatusCode::FORBIDDEN);
        assert_eq!(body["error"]["code"], "device_not_allowed");
    }

    #[tokio::test]
    async fn heartbeat_rejects_invalid_mode_with_json() {
        let (status, body) = request(
            Method::POST,
            "/api/device/heartbeat",
            Some(
                r#"{
                "device_id":"lumahome-01",
                "boot_id":1,
                "mode":"vacation",
                "light_on":false,
                "sensor_raw":0,
                "config_version":1
            }"#,
            ),
        )
        .await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "unsupported_mode");
    }

    #[tokio::test]
    async fn malformed_heartbeat_json_returns_json_bad_request() {
        let (status, body) = request(
            Method::POST,
            "/api/device/heartbeat",
            Some(r#"{"device_id":}"#),
        )
        .await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_json");
    }

    #[tokio::test]
    async fn mode_changes_are_versioned_and_idempotent() {
        let (app, _database) = test_app().await;
        let first = app
            .clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#))
            .await
            .unwrap();
        let first_body = response_json(first).await;
        let repeated = app
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#))
            .await
            .unwrap();
        let repeated_body = response_json(repeated).await;

        assert_eq!(first_body["desired"]["config_version"], 2);
        assert_eq!(repeated_body["desired"]["config_version"], 2);
        assert_eq!(repeated_body["desired"]["mode"], "night");
        assert_eq!(repeated_body["desired"]["manual_light_on"], false);
    }

    #[tokio::test]
    async fn manual_light_changes_are_versioned_and_idempotent() {
        let (app, _database) = test_app().await;
        let first = app
            .clone()
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();
        let first_body = response_json(first).await;
        let repeated = app
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();
        let repeated_body = response_json(repeated).await;

        assert_eq!(first_body["desired"]["manual_light_on"], true);
        assert_eq!(first_body["desired"]["config_version"], 2);
        assert_eq!(repeated_body["desired"]["config_version"], 2);
    }

    #[tokio::test]
    async fn manual_light_is_rejected_in_night_mode() {
        let (app, _database) = test_app().await;
        app.clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#))
            .await
            .unwrap();
        let response = app
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();
        let status = response.status();
        let body = response_json(response).await;

        assert_eq!(status, StatusCode::CONFLICT);
        assert_eq!(body["error"]["code"], "manual_control_unavailable");
    }

    #[tokio::test]
    async fn restart_restores_desired_state_but_not_runtime_heartbeat_state() {
        let directory = tempfile::tempdir().unwrap();
        let database_url = sqlite_url(&directory.path().join("restart-test.db"));
        let first = test_app_at(&database_url).await;
        let response = first
            .clone()
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        first
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();

        let restarted = test_app_at(&database_url).await;
        let response = restarted
            .clone()
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let state = response_json(response).await;

        assert_eq!(state["desired"]["mode"], "manual");
        assert_eq!(state["desired"]["manual_light_on"], true);
        assert_eq!(state["desired"]["config_version"], 2);
        assert_eq!(state["online"], false);
        assert_eq!(state["reported"], Value::Null);
        assert_eq!(state["last_seen"], Value::Null);

        let response = restarted
            .oneshot(json_request("/api/device/heartbeat", HEARTBEAT))
            .await
            .unwrap();
        let heartbeat = response_json(response).await;
        assert_eq!(heartbeat["desired"]["manual_light_on"], true);
        assert_eq!(heartbeat["desired"]["config_version"], 2);
    }

    #[tokio::test]
    async fn mode_and_manual_light_updates_are_loaded_from_an_existing_database() {
        let directory = tempfile::tempdir().unwrap();
        let database_url = sqlite_url(&directory.path().join("existing-state-test.db"));
        let first = test_app_at(&database_url).await;
        first
            .clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#))
            .await
            .unwrap();
        first
            .clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"manual"}"#))
            .await
            .unwrap();
        first
            .oneshot(json_request("/api/light", r#"{"on":true}"#))
            .await
            .unwrap();

        let restarted = test_app_at(&database_url).await;
        let response = restarted
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let state = response_json(response).await;

        assert_eq!(state["desired"]["mode"], "manual");
        assert_eq!(state["desired"]["manual_light_on"], true);
        assert_eq!(state["desired"]["config_version"], 4);
    }

    #[tokio::test]
    async fn concurrent_mode_mutations_preserve_monotonic_versions() {
        let (app, _database) = test_app().await;
        let first = app
            .clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"night"}"#));
        let second = app
            .clone()
            .oneshot(json_request("/api/mode", r#"{"mode":"manual"}"#));
        let (first, second) = tokio::join!(first, second);
        let first = response_json(first.unwrap()).await;
        let second = response_json(second.unwrap()).await;
        let response = app
            .oneshot(
                Request::builder()
                    .uri("/api/state")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let final_state = response_json(response).await;

        let first_version = first["desired"]["config_version"].as_u64().unwrap();
        let second_version = second["desired"]["config_version"].as_u64().unwrap();
        let final_version = final_state["desired"]["config_version"].as_u64().unwrap();
        assert_ne!(first_version, second_version);
        assert_eq!(final_version, first_version.max(second_version));
        assert!((2..=3).contains(&final_version));
    }

    #[tokio::test]
    async fn unsupported_mode_returns_json_bad_request() {
        let (status, body) =
            request(Method::POST, "/api/mode", Some(r#"{"mode":"vacation"}"#)).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "unsupported_mode");
    }

    #[tokio::test]
    async fn malformed_json_returns_json_bad_request() {
        let (status, body) = request(Method::POST, "/api/mode", Some(r#"{"mode":}"#)).await;

        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert_eq!(body["error"]["code"], "invalid_json");
    }

    #[tokio::test]
    async fn unknown_api_route_returns_json_not_found() {
        let (status, body) = request(Method::GET, "/api/unknown", None).await;

        assert_eq!(status, StatusCode::NOT_FOUND);
        assert_eq!(body["error"]["code"], "not_found");
    }

    #[tokio::test]
    async fn api_root_returns_json_not_found() {
        let (status, body) = request(Method::GET, "/api", None).await;

        assert_eq!(status, StatusCode::NOT_FOUND);
        assert_eq!(body["error"]["code"], "not_found");
    }

    #[tokio::test]
    async fn health_endpoint_still_succeeds() {
        let (status, body) = request(Method::GET, "/api/health", None).await;

        assert_eq!(status, StatusCode::OK);
        assert_eq!(body["status"], "ok");
    }

    #[tokio::test]
    async fn known_api_route_rejects_wrong_method_with_json() {
        let (status, body) = request(Method::POST, "/api/state", Some("{}")).await;

        assert_eq!(status, StatusCode::METHOD_NOT_ALLOWED);
        assert_eq!(body["error"]["code"], "method_not_allowed");
    }

    fn json_request(uri: &str, body: impl Into<Body>) -> Request<Body> {
        Request::builder()
            .method(Method::POST)
            .uri(uri)
            .header(CONTENT_TYPE, "application/json")
            .body(body.into())
            .unwrap()
    }

    async fn response_json(response: axum::response::Response) -> Value {
        let bytes = to_bytes(response.into_body(), usize::MAX).await.unwrap();
        serde_json::from_slice(&bytes).unwrap()
    }
}
