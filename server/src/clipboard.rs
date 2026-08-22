//! Account-scoped clipboard history. The server retains text and image previews,
//! never the original image bytes.

use axum::extract::{Path, Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use base64::{engine::general_purpose::STANDARD, Engine};
use chrono::Duration as ChronoDuration;
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};

use crate::agent::broadcast_clipboard;
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

fn image_mime(value: &str) -> bool {
    value.starts_with("image/")
        && value.len() <= 127
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'/' | b'+' | b'-' | b'.'))
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

/// Removes expired records before every operation. Expiry is also a history
/// change, so clients polling with `after` can discard a stale local item.
fn purge_expired(conn: &Connection, user_id: i64, stamp: &str) -> rusqlite::Result<()> {
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

/// Used by session setup: image access is only ever requested from the exact
/// source device recorded with the clipboard metadata.
pub fn image_belongs_to_source(
    conn: &Connection,
    user_id: i64,
    item_id: &str,
    device_id: &str,
    stamp: &str,
) -> Result<(), ApiError> {
    let source: Option<String> = conn
        .query_row(
            "SELECT source_device_id FROM clipboard_items WHERE id = ?1 AND user_id = ?2 \
             AND kind = 'image' AND expires > ?3",
            params![item_id, user_id, stamp],
            |row| row.get(0),
        )
        .optional()
        .unwrap_or(None);
    match source {
        Some(source) if source == device_id => Ok(()),
        Some(_) => Err(fail(
            StatusCode::BAD_REQUEST,
            "clipboard item is not from target device",
        )),
        None => Err(fail(StatusCode::NOT_FOUND, "no such clipboard image")),
    }
}
