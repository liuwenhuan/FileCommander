//! Accounts, sessions and the device registry.
//!
//! Every error body is `{"detail": "..."}`, which is what the Qt client reads
//! to build the message it shows -- keep that shape.

use std::net::SocketAddr;
use std::time::{Duration, Instant};

use axum::extract::{ConnectInfo, Path, Request, State};
use axum::http::{HeaderMap, StatusCode};
use axum::middleware::Next;
use axum::response::{IntoResponse, Response};
use axum::Json;
use chrono::Duration as ChronoDuration;
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::db::{
    constant_time_eq, hash_password, hash_token, iso, now, random_hex, random_token,
};
use crate::AppState;

/// Short enough that a stolen access token is worth little, long enough that a
/// device does not spend its life refreshing.
const ACCESS_TTL_MINUTES: i64 = 15;
const REFRESH_TTL_DAYS: i64 = 90;

/// A device counts as online if its agent said something within this window;
/// the agent pings well inside it.
pub const ONLINE_WINDOW_SECONDS: i64 = 90;

/// Salt used to hash a password for an email that does not exist, so "no such
/// account" costs about as much time as "wrong password".
const DUMMY_SALT: &str = "00000000000000000000000000000000";

const RATE_WINDOW: Duration = Duration::from_secs(60);
const RATE_MAX_ATTEMPTS: u32 = 20;

pub struct ApiError(pub StatusCode, pub String);

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.0, Json(json!({ "detail": self.1 }))).into_response()
    }
}

pub fn fail(code: StatusCode, detail: &str) -> ApiError {
    ApiError(code, detail.to_string())
}

/// The caller behind a bearer token.
pub struct Principal {
    pub user_id: i64,
    pub device_id: String,
}

/// Resolves `Authorization: Bearer <access token>`. Blocking DB work, so it is
/// always awaited from a handler, never from a middleware on the hot path.
pub async fn authenticate(state: &AppState, headers: &HeaderMap) -> Result<Principal, ApiError> {
    let token = headers
        .get("authorization")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
        .ok_or_else(|| fail(StatusCode::UNAUTHORIZED, "missing bearer token"))?;

    let hashed = hash_token(&token);
    let stamp = iso(now());
    state
        .db
        .call(move |conn| {
            let row: Option<(i64, String)> = conn
                .query_row(
                    "SELECT user_id, device_id FROM tokens \
                     WHERE token_hash = ?1 AND kind = 'access' AND expires > ?2",
                    params![hashed, stamp],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .optional()
                .unwrap_or(None);
            match row {
                Some((user_id, device_id)) => Ok(Principal { user_id, device_id }),
                None => Err(fail(StatusCode::UNAUTHORIZED, "invalid or expired token")),
            }
        })
        .await
}

/// Per-address throttle on the credential endpoints. Normally a reverse proxy's
/// job; this server is deployed without one, so it does it itself.
pub async fn rate_limit(State(state): State<AppState>, req: Request, next: Next) -> Response {
    // Without a connect-info extension there is no address to key on (a router
    // driven directly by a test, say). One shared bucket then -- still bounded,
    // never unlimited.
    let key = req
        .extensions()
        .get::<ConnectInfo<SocketAddr>>()
        .map(|info| info.0.ip().to_string())
        .unwrap_or_else(|| "unknown".to_string());

    let allowed = {
        let mut attempts = state.attempts.lock().unwrap_or_else(|e| e.into_inner());
        let clock = Instant::now();
        // Cheap sweep: without it the map grows one entry per address forever.
        if attempts.len() > 1024 {
            attempts.retain(|_, (_, started)| clock.duration_since(*started) < RATE_WINDOW);
        }
        let entry = attempts.entry(key).or_insert((0, clock));
        if clock.duration_since(entry.1) >= RATE_WINDOW {
            *entry = (0, clock);
        }
        entry.0 += 1;
        entry.0 <= RATE_MAX_ATTEMPTS
    };

    if !allowed {
        return fail(StatusCode::TOO_MANY_REQUESTS, "too many attempts, try again later")
            .into_response();
    }
    next.run(req).await
}

#[derive(Deserialize)]
pub struct RegisterRequest {
    #[serde(default)]
    pub email: String,
    #[serde(default)]
    pub password: String,
}

fn clean_email(raw: &str) -> Result<String, ApiError> {
    let email = raw.trim().to_lowercase();
    // Leading/trailing '@' does not make an address: require one in the middle.
    if email.len() < 3 || email.len() > 254 || !email.trim_matches('@').contains('@') {
        return Err(fail(StatusCode::BAD_REQUEST, "invalid email address"));
    }
    Ok(email)
}

fn check_password(password: &str) -> Result<(), ApiError> {
    if password.len() < 8 || password.len() > 128 {
        return Err(fail(
            StatusCode::BAD_REQUEST,
            "password must be between 8 and 128 characters",
        ));
    }
    Ok(())
}

pub async fn register(
    State(state): State<AppState>,
    Json(body): Json<RegisterRequest>,
) -> Result<Response, ApiError> {
    let email = clean_email(&body.email)?;
    check_password(&body.password)?;
    let password = body.password;

    state
        .db
        .call(move |conn| {
            let taken: bool = conn
                .query_row("SELECT 1 FROM users WHERE email = ?1", params![email], |_| Ok(()))
                .optional()
                .unwrap_or(None)
                .is_some();
            if taken {
                return Err(fail(StatusCode::CONFLICT, "email already registered"));
            }
            let salt = random_hex(16);
            let hash = hash_password(&password, &salt);
            conn.execute(
                "INSERT INTO users (email, pw_hash, salt, created) VALUES (?1, ?2, ?3, ?4)",
                params![email, hash, salt, iso(now())],
            )
            .map_err(|_| fail(StatusCode::CONFLICT, "email already registered"))?;
            Ok((StatusCode::CREATED, Json(json!({ "email": email }))).into_response())
        })
        .await
}

#[derive(Deserialize)]
pub struct LoginRequest {
    #[serde(default)]
    pub email: String,
    #[serde(default)]
    pub password: String,
    #[serde(default)]
    pub device_name: String,
    #[serde(default)]
    pub platform: String,
    #[serde(default)]
    pub device_id: String,
}

#[derive(Serialize)]
pub struct TokenPair {
    pub access_token: String,
    pub refresh_token: String,
    pub device_id: String,
    pub expires_in: i64,
}

/// Replaces whatever the device had and hands back a fresh pair. One live
/// session per device: a second sign-in on the same machine invalidates the
/// first, so a re-login after a token leak actually revokes something.
fn issue_tokens(conn: &Connection, user_id: i64, device_id: &str) -> TokenPair {
    let _ = conn.execute("DELETE FROM tokens WHERE device_id = ?1", params![device_id]);
    let access = random_token();
    let refresh = random_token();
    let _ = conn.execute(
        "INSERT INTO tokens (token_hash, user_id, device_id, kind, expires) \
         VALUES (?1, ?2, ?3, 'access', ?4)",
        params![
            hash_token(&access),
            user_id,
            device_id,
            iso(now() + ChronoDuration::minutes(ACCESS_TTL_MINUTES))
        ],
    );
    let _ = conn.execute(
        "INSERT INTO tokens (token_hash, user_id, device_id, kind, expires) \
         VALUES (?1, ?2, ?3, 'refresh', ?4)",
        params![
            hash_token(&refresh),
            user_id,
            device_id,
            iso(now() + ChronoDuration::days(REFRESH_TTL_DAYS))
        ],
    );
    TokenPair {
        access_token: access,
        refresh_token: refresh,
        device_id: device_id.to_string(),
        expires_in: ACCESS_TTL_MINUTES * 60,
    }
}

pub async fn login(
    State(state): State<AppState>,
    Json(body): Json<LoginRequest>,
) -> Result<Json<TokenPair>, ApiError> {
    let email = body.email.trim().to_lowercase();
    let password = body.password;
    let name = if body.device_name.trim().is_empty() {
        "unnamed device".to_string()
    } else {
        body.device_name.trim().to_string()
    };
    let platform = if body.platform.trim().is_empty() {
        "unknown".to_string()
    } else {
        body.platform.trim().to_string()
    };
    let claimed = body.device_id.trim().to_string();

    state
        .db
        .call(move |conn| {
            let found: Option<(i64, String, String)> = conn
                .query_row(
                    "SELECT id, pw_hash, salt FROM users WHERE email = ?1",
                    params![email],
                    |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
                )
                .optional()
                .unwrap_or(None);

            // Same message and roughly the same cost either way: an unknown
            // email must not be distinguishable from a wrong password.
            let invalid = || fail(StatusCode::UNAUTHORIZED, "invalid credentials");
            let (user_id, stored, salt) = match found {
                Some(row) => row,
                None => {
                    hash_password(&password, DUMMY_SALT);
                    return Err(invalid());
                }
            };
            if !constant_time_eq(&hash_password(&password, &salt), &stored) {
                return Err(invalid());
            }

            let stamp = iso(now());
            // Re-claim the device row this install already owns, so signing in
            // twice does not litter the account with duplicate devices.
            let owned = !claimed.is_empty()
                && conn
                    .query_row(
                        "SELECT 1 FROM devices WHERE id = ?1 AND user_id = ?2",
                        params![claimed, user_id],
                        |_| Ok(()),
                    )
                    .optional()
                    .unwrap_or(None)
                    .is_some();
            let device_id = if owned {
                let _ = conn.execute(
                    // last_seen belongs to the agent socket alone: a device is
                    // "online" only while that socket is reporting, never merely
                    // because someone signed in on it.
                    "UPDATE devices SET name = ?1, platform = ?2 WHERE id = ?3",
                    params![name, platform, claimed],
                );
                claimed
            } else {
                let id = random_hex(16);
                conn.execute(
                    "INSERT INTO devices (id, user_id, name, platform, created) \
                     VALUES (?1, ?2, ?3, ?4, ?5)",
                    params![id, user_id, name, platform, stamp],
                )
                .map_err(|_| {
                    fail(StatusCode::INTERNAL_SERVER_ERROR, "could not register device")
                })?;
                id
            };
            Ok(Json(issue_tokens(conn, user_id, &device_id)))
        })
        .await
}

#[derive(Deserialize)]
pub struct RefreshRequest {
    #[serde(default)]
    pub refresh_token: String,
}

pub async fn refresh(
    State(state): State<AppState>,
    Json(body): Json<RefreshRequest>,
) -> Result<Json<TokenPair>, ApiError> {
    let hashed = hash_token(body.refresh_token.trim());
    let stamp = iso(now());

    state
        .db
        .call(move |conn| {
            let row: Option<(i64, String)> = conn
                .query_row(
                    "SELECT user_id, device_id FROM tokens \
                     WHERE token_hash = ?1 AND kind = 'refresh' AND expires > ?2",
                    params![hashed, stamp],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .optional()
                .unwrap_or(None);
            let (user_id, device_id) = row
                .ok_or_else(|| fail(StatusCode::UNAUTHORIZED, "invalid refresh token"))?;
            // Rotation: the presented refresh token is spent. issue_tokens
            // clears the rest of the device's tokens with it.
            Ok(Json(issue_tokens(conn, user_id, &device_id)))
        })
        .await
}

pub async fn logout(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<StatusCode, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let device_id = principal.device_id.clone();
    state.agents.lock().unwrap_or_else(|e| e.into_inner()).remove(&device_id);
    state
        .db
        .call(move |conn| {
            let _ = conn.execute("DELETE FROM tokens WHERE device_id = ?1", params![device_id]);
        })
        .await;
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Serialize)]
pub struct DeviceView {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub online: bool,
    pub lan_addrs: Vec<String>,
    pub last_seen: String,
    #[serde(rename = "self")]
    pub is_self: bool,
}

pub async fn devices(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<DeviceView>>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let this_device = principal.device_id.clone();
    let user_id = principal.user_id;
    let cutoff = iso(now() - ChronoDuration::seconds(ONLINE_WINDOW_SECONDS));

    let rows = state
        .db
        .call(move |conn| {
            // `created` is only accurate to the second, so two devices signed
            // in from the same machine land on the same value and the tiebreak
            // decides the order the user sees. It has to be rowid, not id: id
            // is random hex, which would shuffle the list on every call.
            let mut stmt = match conn.prepare(
                "SELECT id, name, platform, last_seen, lan_addrs FROM devices \
                 WHERE user_id = ?1 ORDER BY created, rowid",
            ) {
                Ok(stmt) => stmt,
                Err(_) => return Vec::new(),
            };
            let mapped = stmt.query_map(params![user_id], |r| {
                let id: String = r.get(0)?;
                let last_seen: Option<String> = r.get(3)?;
                let addrs: String = r.get(4)?;
                Ok(DeviceView {
                    online: last_seen.as_deref().map(|s| s >= cutoff.as_str()).unwrap_or(false),
                    is_self: id == this_device,
                    id,
                    name: r.get(1)?,
                    platform: r.get(2)?,
                    lan_addrs: addrs
                        .split(',')
                        .filter(|s| !s.is_empty())
                        .map(|s| s.to_string())
                        .collect(),
                    last_seen: last_seen.unwrap_or_default(),
                })
            });
            match mapped {
                Ok(iter) => iter.filter_map(|r| r.ok()).collect(),
                Err(_) => Vec::new(),
            }
        })
        .await;
    Ok(Json(rows))
}

pub async fn forget_device(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(device_id): Path<String>,
) -> Result<StatusCode, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    state.agents.lock().unwrap_or_else(|e| e.into_inner()).remove(&device_id);

    state
        .db
        .call(move |conn| {
            let removed = conn
                .execute(
                    "DELETE FROM devices WHERE id = ?1 AND user_id = ?2",
                    params![device_id, user_id],
                )
                .unwrap_or(0);
            if removed == 0 {
                return Err(fail(StatusCode::NOT_FOUND, "no such device"));
            }
            // A device nobody can reach must not keep a usable session.
            let _ = conn.execute("DELETE FROM tokens WHERE device_id = ?1", params![device_id]);
            Ok(StatusCode::NO_CONTENT)
        })
        .await
}
