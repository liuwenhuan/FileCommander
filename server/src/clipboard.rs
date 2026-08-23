//! Account-scoped clipboard history and explicit device deliveries. History keeps
//! text and image previews; explicit deliveries retain original bytes until every
//! recipient acknowledges them or their retention window expires.

use axum::body::to_bytes;
use axum::extract::{Path, Query, Request, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use base64::{engine::general_purpose::STANDARD, Engine};
use chrono::Duration as ChronoDuration;
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::agent::{
    broadcast_clipboard, broadcast_clipboard_delivery, ClipboardDeliveryMetadata,
    ClipboardDeliveryTarget,
};
use crate::api::{authenticate, fail, ApiError};
use crate::db::{iso, now, random_hex};
use crate::AppState;

const HISTORY_LIMIT: usize = 20;
const TTL_DAYS: i64 = 7;
const MAX_TEXT_BYTES: usize = 64 * 1024;
const MAX_THUMBNAIL_BYTES: usize = 128 * 1024;
const MAX_IMAGE_BYTES: u64 = 25 * 1024 * 1024;
const MAX_IMAGE_DIMENSION: u64 = 16_384;
const MAX_IMAGE_PIXELS: u64 = 40_000_000;

#[derive(Deserialize)]
pub struct ListQuery {
    pub after: Option<i64>,
}

#[derive(Deserialize)]
pub struct PublishRequest {
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub thumbnail_base64: String,
    #[serde(default)]
    pub thumbnail_mime: String,
    #[serde(default)]
    pub mime: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub width: u64,
    #[serde(default)]
    pub height: u64,
    #[serde(default)]
    pub sha256: String,
    #[serde(default)]
    pub source_device_id: String,
}

#[derive(Serialize)]
pub struct ClipboardItemView {
    pub id: String,
    pub revision: i64,
    #[serde(rename = "type")]
    pub kind: String,
    pub created: String,
    pub expires: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mime: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub size: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub width: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub height: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_device_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub thumbnail_mime: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub thumbnail_url: Option<String>,
}

#[derive(Serialize)]
pub struct ClipboardList {
    pub revision: i64,
    pub items: Vec<ClipboardItemView>,
    pub deleted_ids: Vec<String>,
    pub cleared: bool,
}

#[derive(Serialize)]
pub struct ChangeResponse {
    pub revision: i64,
}

#[derive(Serialize)]
pub struct DeliveryView {
    pub id: String,
    pub payload_id: String,
    pub kind: String,
    pub mime: String,
    pub size: i64,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub sha256: String,
    pub source_device_id: String,
    pub source_device_name: String,
    pub created: String,
    pub expires: String,
}

#[derive(Serialize)]
pub struct DeliveryList {
    pub deliveries: Vec<DeliveryView>,
}

#[derive(Serialize)]
pub struct SendDeliveryResponse {
    pub payload_id: String,
    pub recipient_count: usize,
}

struct DeliveryPayload {
    kind: String,
    mime: String,
    content: Vec<u8>,
    size: i64,
    width: Option<i64>,
    height: Option<i64>,
    sha256: String,
}

fn image_mime(value: &str) -> bool {
    let Some((media_type, subtype)) = value.split_once('/') else {
        return false;
    };
    value.len() <= 127
        && media_type.eq_ignore_ascii_case("image")
        && !subtype.is_empty()
        && !subtype.contains('/')
        && subtype
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'+' | b'-' | b'.'))
}

fn sha256(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn next_revision(conn: &Connection, user_id: i64) -> rusqlite::Result<i64> {
    conn.execute(
        "INSERT OR IGNORE INTO clipboard_state (user_id, revision) VALUES (?1, 0)",
        params![user_id],
    )?;
    conn.execute(
        "UPDATE clipboard_state SET revision = revision + 1 WHERE user_id = ?1",
        params![user_id],
    )?;
    conn.query_row(
        "SELECT revision FROM clipboard_state WHERE user_id = ?1",
        params![user_id],
        |row| row.get(0),
    )
}

fn current_revision(conn: &Connection, user_id: i64) -> i64 {
    conn.query_row(
        "SELECT revision FROM clipboard_state WHERE user_id = ?1",
        params![user_id],
        |row| row.get(0),
    )
    .optional()
    .unwrap_or(None)
    .unwrap_or(0)
}

fn event(
    conn: &Connection,
    user_id: i64,
    revision: i64,
    kind: &str,
    item_id: Option<&str>,
    expires: &str,
) -> rusqlite::Result<()> {
    conn.execute(
        "INSERT INTO clipboard_events (user_id, revision, kind, item_id, expires) \
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![user_id, revision, kind, item_id, expires],
    )?;
    Ok(())
}

/// Removes expired explicit-delivery content for every account. This runs both
/// from request handling and from the process-wide background task, so an idle
/// account never keeps original clipboard content beyond the retention window.
pub fn purge_expired_delivery_payloads(conn: &Connection, stamp: &str) -> rusqlite::Result<()> {
    conn.execute(
        "DELETE FROM clipboard_deliveries WHERE payload_id IN \
         (SELECT id FROM clipboard_payloads WHERE expires <= ?1)",
        params![stamp],
    )?;
    conn.execute(
        "DELETE FROM clipboard_payloads WHERE expires <= ?1",
        params![stamp],
    )?;
    // A completed delivery's row stays as an ACK tombstone, but its payload has
    // no reason to survive once every recipient is no longer pending. This also
    // repairs any pre-transaction partial completion from an older server.
    conn.execute(
        "DELETE FROM clipboard_payloads WHERE NOT EXISTS \
         (SELECT 1 FROM clipboard_deliveries WHERE payload_id = clipboard_payloads.id \
          AND state = 'pending')",
        [],
    )?;
    // Keep a delivered row only for the same seven-day window as a replay-safe
    // ACK. It authorizes retrying an ACK without retaining metadata forever.
    let delivered_cutoff = iso(now() - ChronoDuration::days(TTL_DAYS));
    conn.execute(
        "DELETE FROM clipboard_deliveries WHERE state = 'delivered' \
         AND delivered_at IS NOT NULL AND delivered_at <= ?1",
        params![delivered_cutoff],
    )?;
    Ok(())
}

/// Removes expired records before every account-scoped history operation.
/// Expiry is also a history change, so clients polling with `after` can discard
/// a stale local item.
fn purge_expired(conn: &Connection, user_id: i64, stamp: &str) -> rusqlite::Result<()> {
    purge_expired_delivery_payloads(conn, stamp)?;
    conn.execute(
        "DELETE FROM clipboard_events WHERE expires <= ?1",
        params![stamp],
    )?;
    let mut stmt =
        conn.prepare("SELECT id FROM clipboard_items WHERE user_id = ?1 AND expires <= ?2")?;
    let ids: Vec<String> = stmt
        .query_map(params![user_id, stamp], |row| row.get(0))?
        .filter_map(Result::ok)
        .collect();
    drop(stmt);
    if ids.is_empty() {
        return Ok(());
    }
    let change_revision = next_revision(conn, user_id)?;
    let event_expiry = iso(now() + ChronoDuration::days(TTL_DAYS));
    for id in ids {
        conn.execute(
            "DELETE FROM clipboard_items WHERE id = ?1 AND user_id = ?2",
            params![id, user_id],
        )?;
        event(
            conn,
            user_id,
            change_revision,
            "delete",
            Some(&id),
            &event_expiry,
        )?;
    }
    Ok(())
}

fn item_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<ClipboardItemView> {
    let id: String = row.get(0)?;
    let kind: String = row.get(2)?;
    Ok(ClipboardItemView {
        thumbnail_url: (kind == "image").then(|| format!("/v1/clipboard/{id}/thumbnail")),
        id,
        revision: row.get(1)?,
        kind,
        created: row.get(10)?,
        expires: row.get(11)?,
        text: row.get(3)?,
        mime: row.get(4)?,
        size: row.get(5)?,
        width: row.get(6)?,
        height: row.get(7)?,
        sha256: row.get(8)?,
        source_device_id: row.get(9)?,
        thumbnail_mime: row.get(12)?,
    })
}

fn fetch_items(conn: &Connection, user_id: i64, after: Option<i64>) -> Vec<ClipboardItemView> {
    let sql = if after.is_some() {
        "SELECT id, revision, kind, text, image_mime, image_size, image_width, image_height, \
         image_sha256, source_device_id, created, expires, thumbnail_mime \
         FROM clipboard_items WHERE user_id = ?1 AND revision > ?2 ORDER BY revision"
    } else {
        "SELECT id, revision, kind, text, image_mime, image_size, image_width, image_height, \
         image_sha256, source_device_id, created, expires, thumbnail_mime \
         FROM clipboard_items WHERE user_id = ?1 ORDER BY created DESC, rowid DESC LIMIT 20"
    };
    let Ok(mut stmt) = conn.prepare(sql) else {
        return Vec::new();
    };
    let rows = if let Some(after) = after {
        stmt.query_map(params![user_id, after], item_from_row)
    } else {
        stmt.query_map(params![user_id], item_from_row)
    };
    rows.map(|rows| rows.filter_map(Result::ok).collect())
        .unwrap_or_default()
}

pub async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<ListQuery>,
) -> Result<Json<ClipboardList>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let after = query.after.map(|value| value.max(0));
    let user_id = principal.user_id;
    let stamp = iso(now());
    let response = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp)
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not read clipboard"))?;
            let current = current_revision(conn, user_id);
            let deleted_ids = if let Some(after) = after {
                let mut stmt = conn
                    .prepare(
                        "SELECT item_id FROM clipboard_events WHERE user_id = ?1 AND revision > ?2 \
                         AND kind = 'delete' AND item_id IS NOT NULL ORDER BY revision",
                    )
                    .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not read clipboard"))?;
                stmt.query_map(params![user_id, after], |row| row.get(0))
                    .map(|rows| rows.filter_map(Result::ok).collect())
                    .unwrap_or_default()
            } else {
                Vec::new()
            };
            let cleared = after.map(|after| {
                conn.query_row(
                    "SELECT 1 FROM clipboard_events WHERE user_id = ?1 AND revision > ?2 \
                     AND kind = 'clear' LIMIT 1",
                    params![user_id, after],
                    |_| Ok(()),
                )
                .optional()
                .unwrap_or(None)
                .is_some()
            }).unwrap_or(false);
            Ok(Json(ClipboardList {
                revision: current,
                items: fetch_items(conn, user_id, after),
                deleted_ids,
                cleared,
            }))
        })
        .await?;
    Ok(response)
}

pub async fn publish(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(body): Json<PublishRequest>,
) -> Result<Json<ClipboardItemView>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let source_device = principal.device_id.clone();
    let id = random_hex(16);
    let created = iso(now());
    let expires = iso(now() + ChronoDuration::days(TTL_DAYS));

    let (kind, text, thumbnail, thumbnail_mime, mime, size, width, height, sha256, source) =
        match body.kind.as_str() {
            "text" => {
                if body.text.as_bytes().len() > MAX_TEXT_BYTES {
                    return Err(fail(
                        StatusCode::PAYLOAD_TOO_LARGE,
                        "clipboard text is too large",
                    ));
                }
                (
                    "text".to_string(),
                    Some(body.text),
                    None,
                    None,
                    None,
                    None,
                    None,
                    None,
                    None,
                    Some(source_device.clone()),
                )
            }
            "image" => {
                if body.source_device_id.trim() != source_device
                    || !image_mime(&body.mime)
                    || !image_mime(&body.thumbnail_mime)
                    || body.size > MAX_IMAGE_BYTES
                    || body.width == 0
                    || body.height == 0
                    || body.width > MAX_IMAGE_DIMENSION
                    || body.height > MAX_IMAGE_DIMENSION
                    || body.width.checked_mul(body.height).unwrap_or(u64::MAX) > MAX_IMAGE_PIXELS
                    || !sha256(&body.sha256)
                {
                    return Err(fail(StatusCode::BAD_REQUEST, "invalid clipboard image"));
                }
                let thumbnail = STANDARD
                    .decode(body.thumbnail_base64.as_bytes())
                    .map_err(|_| fail(StatusCode::BAD_REQUEST, "invalid clipboard image"))?;
                if thumbnail.len() > MAX_THUMBNAIL_BYTES {
                    return Err(fail(
                        StatusCode::PAYLOAD_TOO_LARGE,
                        "clipboard thumbnail is too large",
                    ));
                }
                (
                    "image".to_string(),
                    None,
                    Some(thumbnail),
                    Some(body.thumbnail_mime),
                    Some(body.mime),
                    Some(body.size as i64),
                    Some(body.width as i64),
                    Some(body.height as i64),
                    Some(body.sha256.to_lowercase()),
                    Some(source_device.clone()),
                )
            }
            _ => return Err(fail(StatusCode::BAD_REQUEST, "invalid clipboard type")),
        };

    let item = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &created)
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
            let change_revision = next_revision(conn, user_id)
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
            conn.execute(
                "INSERT INTO clipboard_items (id, user_id, kind, text, thumbnail, thumbnail_mime, \
                 image_mime, image_size, image_width, image_height, image_sha256, source_device_id, \
                 revision, created, expires) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, \
                 ?12, ?13, ?14, ?15)",
                params![id, user_id, kind, text, thumbnail, thumbnail_mime, mime, size, width, height,
                    sha256, source, change_revision, created, expires],
            )
            .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
            event(conn, user_id, change_revision, "publish", Some(&id), &expires)
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;

            let mut stmt = conn
                .prepare("SELECT id FROM clipboard_items WHERE user_id = ?1 \
                          ORDER BY created DESC, rowid DESC LIMIT -1 OFFSET ?2")
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
            let evicted: Vec<String> = stmt
                .query_map(params![user_id, HISTORY_LIMIT as i64], |row| row.get(0))
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?
                .filter_map(Result::ok)
                .collect();
            drop(stmt);
            for stale in evicted {
                conn.execute(
                    "DELETE FROM clipboard_items WHERE id = ?1 AND user_id = ?2",
                    params![stale, user_id],
                )
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
                event(conn, user_id, change_revision, "delete", Some(&stale), &expires)
                    .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))?;
            }
            conn.query_row(
                "SELECT id, revision, kind, text, image_mime, image_size, image_width, image_height, \
                 image_sha256, source_device_id, created, expires, thumbnail_mime \
                 FROM clipboard_items WHERE id = ?1 AND user_id = ?2",
                params![id, user_id],
                item_from_row,
            )
            .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not save clipboard"))
        })
        .await?;
    broadcast_clipboard(
        &state,
        user_id,
        &principal.device_id,
        item.revision,
        "publish",
    );
    Ok(Json(item))
}

pub async fn delete_item(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(item_id): Path<String>,
) -> Result<Json<ChangeResponse>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let stamp = iso(now());
    let expires = iso(now() + ChronoDuration::days(TTL_DAYS));
    let revision = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not delete clipboard item",
                )
            })?;
            let exists = conn
                .query_row(
                    "SELECT 1 FROM clipboard_items WHERE id = ?1 AND user_id = ?2",
                    params![item_id, user_id],
                    |_| Ok(()),
                )
                .optional()
                .unwrap_or(None)
                .is_some();
            if !exists {
                return Err(fail(StatusCode::NOT_FOUND, "no such clipboard item"));
            }
            let change_revision = next_revision(conn, user_id).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not delete clipboard item",
                )
            })?;
            conn.execute(
                "DELETE FROM clipboard_items WHERE id = ?1 AND user_id = ?2",
                params![item_id, user_id],
            )
            .map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not delete clipboard item",
                )
            })?;
            event(
                conn,
                user_id,
                change_revision,
                "delete",
                Some(&item_id),
                &expires,
            )
            .map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not delete clipboard item",
                )
            })?;
            Ok(change_revision)
        })
        .await?;
    broadcast_clipboard(&state, user_id, &principal.device_id, revision, "delete");
    Ok(Json(ChangeResponse { revision }))
}

pub async fn clear(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<ChangeResponse>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let stamp = iso(now());
    let expires = iso(now() + ChronoDuration::days(TTL_DAYS));
    let changed = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not clear clipboard",
                )
            })?;
            let count: i64 = conn
                .query_row(
                    "SELECT COUNT(*) FROM clipboard_items WHERE user_id = ?1",
                    params![user_id],
                    |row| row.get(0),
                )
                .unwrap_or(0);
            if count == 0 {
                return Ok(None);
            }
            let change_revision = next_revision(conn, user_id).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not clear clipboard",
                )
            })?;
            conn.execute(
                "DELETE FROM clipboard_items WHERE user_id = ?1",
                params![user_id],
            )
            .map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not clear clipboard",
                )
            })?;
            event(conn, user_id, change_revision, "clear", None, &expires).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not clear clipboard",
                )
            })?;
            Ok(Some(change_revision))
        })
        .await?;
    let revision = changed.unwrap_or_else(|| {
        // The read is only needed for an idempotent clear; it never alters state.
        0
    });
    let revision = if revision == 0 {
        let user_id = principal.user_id;
        state
            .db
            .call(move |conn| current_revision(conn, user_id))
            .await
    } else {
        revision
    };
    if changed.is_some() {
        broadcast_clipboard(
            &state,
            principal.user_id,
            &principal.device_id,
            revision,
            "clear",
        );
    }
    Ok(Json(ChangeResponse { revision }))
}

pub async fn send_delivery(
    State(state): State<AppState>,
    request: Request,
) -> Result<Json<SendDeliveryResponse>, ApiError> {
    let (parts, body) = request.into_parts();
    let principal = authenticate(&state, &parts.headers).await?;
    let user_id = principal.user_id;
    let source_device_id = principal.device_id;
    let stamp = iso(now());
    state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not save clipboard delivery",
                )
            })
        })
        .await?;

    let content_type = parts
        .headers
        .get(header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .map(str::to_owned)
        .ok_or_else(|| {
            fail(
                StatusCode::UNSUPPORTED_MEDIA_TYPE,
                "invalid clipboard content type",
            )
        })?;
    let payload = if content_type == "text/plain; charset=utf-8" {
        let body = to_bytes(body, MAX_TEXT_BYTES)
            .await
            .map_err(|_| fail(StatusCode::PAYLOAD_TOO_LARGE, "clipboard text is too large"))?;
        if body.is_empty() {
            return Err(fail(StatusCode::BAD_REQUEST, "clipboard text is empty"));
        }
        if std::str::from_utf8(&body).is_err() {
            return Err(fail(
                StatusCode::BAD_REQUEST,
                "clipboard text must be UTF-8",
            ));
        }
        DeliveryPayload {
            kind: "text".to_string(),
            mime: content_type,
            size: body.len() as i64,
            sha256: hex::encode(Sha256::digest(&body)),
            content: body.to_vec(),
            width: None,
            height: None,
        }
    } else if image_mime(&content_type) && content_type.len() > "image/".len() {
        let width = parts
            .headers
            .get("x-clipboard-width")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.parse::<u64>().ok());
        let height = parts
            .headers
            .get("x-clipboard-height")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.parse::<u64>().ok());
        let expected_sha256 = parts
            .headers
            .get("x-clipboard-sha256")
            .and_then(|value| value.to_str().ok())
            .filter(|value| sha256(value))
            .map(str::to_ascii_lowercase);
        let (width, height, expected_sha256) = match (width, height, expected_sha256) {
            (Some(width), Some(height), Some(expected_sha256))
                if width > 0
                    && height > 0
                    && width <= MAX_IMAGE_DIMENSION
                    && height <= MAX_IMAGE_DIMENSION
                    && width.checked_mul(height).unwrap_or(u64::MAX) <= MAX_IMAGE_PIXELS =>
            {
                (width, height, expected_sha256)
            }
            _ => return Err(fail(StatusCode::BAD_REQUEST, "invalid clipboard image")),
        };
        let body = to_bytes(body, MAX_IMAGE_BYTES as usize)
            .await
            .map_err(|_| {
                fail(
                    StatusCode::PAYLOAD_TOO_LARGE,
                    "clipboard image is too large",
                )
            })?;
        let actual_sha256 = hex::encode(Sha256::digest(&body));
        if actual_sha256 != expected_sha256 {
            return Err(fail(StatusCode::BAD_REQUEST, "invalid clipboard image"));
        }
        DeliveryPayload {
            kind: "image".to_string(),
            mime: content_type,
            size: body.len() as i64,
            sha256: actual_sha256,
            content: body.to_vec(),
            width: Some(width as i64),
            height: Some(height as i64),
        }
    } else {
        return Err(fail(
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "clipboard delivery requires text/plain; charset=utf-8 or image/*",
        ));
    };

    let payload_id = random_hex(16);
    let created = iso(now());
    let expires = iso(now() + ChronoDuration::days(TTL_DAYS));
    let delivery_metadata = ClipboardDeliveryMetadata {
        kind: payload.kind.clone(),
        size: payload.size,
    };
    let (response, targets) = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &created).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not save clipboard delivery",
                )
            })?;
            let transaction = conn.unchecked_transaction().map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not save clipboard delivery",
                )
            })?;
            let recipients: Vec<String> = {
                let mut statement = transaction
                    .prepare("SELECT id FROM devices WHERE user_id = ?1 AND id != ?2")
                    .map_err(|_| {
                        fail(
                            StatusCode::INTERNAL_SERVER_ERROR,
                            "could not save clipboard delivery",
                        )
                    })?;
                let rows = statement
                    .query_map(params![user_id, source_device_id], |row| row.get(0))
                    .map_err(|_| {
                        fail(
                            StatusCode::INTERNAL_SERVER_ERROR,
                            "could not save clipboard delivery",
                        )
                    })?;
                rows.collect::<Result<Vec<_>, _>>().map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not save clipboard delivery",
                    )
                })?
            };
            if recipients.is_empty() {
                transaction.commit().map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not save clipboard delivery",
                    )
                })?;
                return Ok((
                    SendDeliveryResponse {
                        payload_id,
                        recipient_count: 0,
                    },
                    Vec::new(),
                ));
            }
            transaction
                .execute(
                    "INSERT INTO clipboard_payloads \
                     (id, user_id, source_device_id, kind, mime, content, size, width, height, \
                      sha256, created, expires) \
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
                    params![
                        payload_id,
                        user_id,
                        source_device_id,
                        payload.kind,
                        payload.mime,
                        payload.content,
                        payload.size,
                        payload.width,
                        payload.height,
                        payload.sha256,
                        created,
                        expires
                    ],
                )
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not save clipboard delivery",
                    )
                })?;
            let mut targets = Vec::with_capacity(recipients.len());
            for target_device_id in recipients {
                let delivery_id = random_hex(16);
                transaction
                    .execute(
                        "INSERT INTO clipboard_deliveries (id, payload_id, target_device_id) \
                         VALUES (?1, ?2, ?3)",
                        params![delivery_id, payload_id, target_device_id],
                    )
                    .map_err(|_| {
                        fail(
                            StatusCode::INTERNAL_SERVER_ERROR,
                            "could not save clipboard delivery",
                        )
                    })?;
                targets.push(ClipboardDeliveryTarget {
                    device_id: target_device_id,
                    delivery_id,
                });
            }
            transaction.commit().map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not save clipboard delivery",
                )
            })?;
            Ok((
                SendDeliveryResponse {
                    payload_id,
                    recipient_count: targets.len(),
                },
                targets,
            ))
        })
        .await?;
    broadcast_clipboard_delivery(&state, user_id, &targets, &delivery_metadata);
    Ok(Json(response))
}

fn delivery_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<DeliveryView> {
    Ok(DeliveryView {
        id: row.get(0)?,
        payload_id: row.get(1)?,
        kind: row.get(2)?,
        mime: row.get(3)?,
        size: row.get(4)?,
        width: row.get(5)?,
        height: row.get(6)?,
        sha256: row.get(7)?,
        source_device_id: row.get(8)?,
        source_device_name: row.get(9)?,
        created: row.get(10)?,
        expires: row.get(11)?,
    })
}

pub async fn deliveries(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<DeliveryList>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let device_id = principal.device_id;
    let stamp = iso(now());
    let deliveries = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not read clipboard deliveries",
                )
            })?;
            let mut statement = conn
                .prepare(
                    "SELECT d.id, d.payload_id, p.kind, p.mime, p.size, p.width, p.height, p.sha256, \
                     p.source_device_id, COALESCE(source.name, ''), p.created, p.expires \
                     FROM clipboard_deliveries d \
                     JOIN clipboard_payloads p ON p.id = d.payload_id \
                     LEFT JOIN devices source ON source.id = p.source_device_id \
                                           AND source.user_id = p.user_id \
                     WHERE d.target_device_id = ?1 AND p.user_id = ?2 AND d.state = 'pending' \
                     ORDER BY p.created DESC, d.rowid DESC",
                )
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not read clipboard deliveries",
                    )
                })?;
            let rows = statement
                .query_map(params![device_id, user_id], delivery_from_row)
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not read clipboard deliveries",
                    )
                })?;
            rows
                .collect::<Result<Vec<_>, _>>()
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not read clipboard deliveries",
                    )
                })
        })
        .await?;
    Ok(Json(DeliveryList { deliveries }))
}

pub async fn delivery_content(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(delivery_id): Path<String>,
) -> Result<Response, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let device_id = principal.device_id;
    let stamp = iso(now());
    let (mime, content, sha256) = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not read clipboard delivery",
                )
            })?;
            conn.query_row(
                "SELECT p.mime, p.content, p.sha256 FROM clipboard_deliveries d \
                 JOIN clipboard_payloads p ON p.id = d.payload_id \
                 WHERE d.id = ?1 AND d.target_device_id = ?2 AND p.user_id = ?3 \
                   AND d.state = 'pending'",
                params![delivery_id, device_id, user_id],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, Vec<u8>>(1)?,
                        row.get::<_, String>(2)?,
                    ))
                },
            )
            .optional()
            .map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not read clipboard delivery",
                )
            })?
            .ok_or_else(|| fail(StatusCode::NOT_FOUND, "no such clipboard delivery"))
        })
        .await?;
    let length = content.len().to_string();
    Ok(Response::builder()
        .header(header::CONTENT_TYPE, mime)
        .header(header::CONTENT_LENGTH, length)
        .header("x-content-sha256", sha256)
        .body(axum::body::Body::from(content))
        .expect("static response headers"))
}

pub async fn acknowledge_delivery(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(delivery_id): Path<String>,
) -> Result<StatusCode, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let device_id = principal.device_id;
    let stamp = iso(now());
    state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp).map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not acknowledge clipboard delivery",
                )
            })?;
            let transaction = conn.unchecked_transaction().map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not acknowledge clipboard delivery",
                )
            })?;
            let payload_id: Option<String> = transaction
                .query_row(
                    "SELECT d.payload_id FROM clipboard_deliveries d \
                     JOIN devices target ON target.id = d.target_device_id AND target.user_id = ?3 \
                     WHERE d.id = ?1 AND d.target_device_id = ?2",
                    params![delivery_id, device_id, user_id],
                    |row| row.get(0),
                )
                .optional()
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not acknowledge clipboard delivery",
                    )
                })?;
            let payload_id = payload_id
                .ok_or_else(|| fail(StatusCode::NOT_FOUND, "no such clipboard delivery"))?;
            transaction
                .execute(
                    "UPDATE clipboard_deliveries SET state = 'delivered', delivered_at = ?1 \
                     WHERE id = ?2 AND target_device_id = ?3 AND state = 'pending'",
                    params![stamp, delivery_id, device_id],
                )
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not acknowledge clipboard delivery",
                    )
                })?;
            transaction
                .execute(
                    "DELETE FROM clipboard_payloads WHERE id = ?1 AND user_id = ?2 AND NOT EXISTS \
                     (SELECT 1 FROM clipboard_deliveries WHERE payload_id = ?1 AND state = 'pending')",
                    params![payload_id, user_id],
                )
                .map_err(|_| {
                    fail(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "could not acknowledge clipboard delivery",
                    )
                })?;
            transaction.commit().map_err(|_| {
                fail(
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "could not acknowledge clipboard delivery",
                )
            })
        })
        .await?;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn thumbnail(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(item_id): Path<String>,
) -> Result<Response, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let stamp = iso(now());
    let value = state
        .db
        .call(move |conn| {
            purge_expired(conn, user_id, &stamp)
                .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not read clipboard thumbnail"))?;
            conn.query_row(
                "SELECT thumbnail_mime, thumbnail FROM clipboard_items WHERE id = ?1 AND user_id = ?2 \
                 AND kind = 'image'",
                params![item_id, user_id],
                |row| Ok((row.get::<_, String>(0)?, row.get::<_, Vec<u8>>(1)?)),
            )
            .optional()
            .map_err(|_| fail(StatusCode::INTERNAL_SERVER_ERROR, "could not read clipboard thumbnail"))?
            .ok_or_else(|| fail(StatusCode::NOT_FOUND, "no such clipboard image"))
        })
        .await?;
    Ok(([(header::CONTENT_TYPE, value.0)], value.1).into_response())
}
