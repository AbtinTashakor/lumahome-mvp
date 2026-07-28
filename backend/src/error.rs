use axum::{http::StatusCode, response::IntoResponse, Json};
use serde::Serialize;

#[derive(Serialize)]
struct ErrorResponse {
    error: ErrorDetail,
}

#[derive(Serialize)]
struct ErrorDetail {
    code: &'static str,
    message: &'static str,
}

pub struct ApiError {
    status: StatusCode,
    code: &'static str,
    message: &'static str,
}

impl ApiError {
    pub const fn invalid_json() -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            code: "invalid_json",
            message: "Request body must be valid JSON with the required fields.",
        }
    }

    pub const fn unsupported_mode() -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            code: "unsupported_mode",
            message: "Mode must be either 'manual' or 'night'.",
        }
    }

    pub const fn manual_control_unavailable() -> Self {
        Self {
            status: StatusCode::CONFLICT,
            code: "manual_control_unavailable",
            message: "Manual light control is unavailable while Night mode is active.",
        }
    }

    pub const fn device_not_allowed() -> Self {
        Self {
            status: StatusCode::FORBIDDEN,
            code: "device_not_allowed",
            message: "This device is not allowed to report to the backend.",
        }
    }

    pub const fn invalid_event_batch() -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            code: "invalid_event_batch",
            message:
                "Events must have positive, strictly increasing sequence numbers and valid values.",
        }
    }

    pub const fn event_batch_too_large() -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            code: "event_batch_too_large",
            message: "A heartbeat may contain at most 32 events.",
        }
    }

    pub const fn invalid_event_limit() -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            code: "invalid_event_limit",
            message: "Event limit must be an integer from 1 through 50.",
        }
    }

    pub const fn not_found() -> Self {
        Self {
            status: StatusCode::NOT_FOUND,
            code: "not_found",
            message: "API route not found.",
        }
    }

    pub const fn method_not_allowed() -> Self {
        Self {
            status: StatusCode::METHOD_NOT_ALLOWED,
            code: "method_not_allowed",
            message: "HTTP method is not allowed for this API route.",
        }
    }

    pub const fn persistence_failed() -> Self {
        Self {
            status: StatusCode::INTERNAL_SERVER_ERROR,
            code: "persistence_failed",
            message: "Persistent data could not be saved or loaded.",
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> axum::response::Response {
        (
            self.status,
            Json(ErrorResponse {
                error: ErrorDetail {
                    code: self.code,
                    message: self.message,
                },
            }),
        )
            .into_response()
    }
}
