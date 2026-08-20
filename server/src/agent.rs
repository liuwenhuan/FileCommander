//! Presence and signalling.
//!
//! Each signed-in device keeps one WebSocket open here. It tells the server how
//! to reach it (LAN addresses, share port); the server pushes it a ticket when
//! another of the account's devices wants to connect. The bytes of a transfer
//! never pass through this socket -- the two devices talk WebDAV directly.

use std::sync::atomic::Ordering;

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Response;
use axum::Json;
use futures_util::{SinkExt, StreamExt};
use rusqlite::{params, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tokio::sync::mpsc;

use crate::api::{authenticate, fail, ApiError};
use crate::db::{iso, now, random_hex, random_token};
use crate::{AgentConn, AppState};

/// How long a device has to use a ticket before the peer forgets it. Long
/// enough to survive a slow LAN probe, short enough that a leaked ticket is
/// worth little.
const TICKET_TTL_SECONDS: i64 = 300;

#[derive(Deserialize)]
pub struct AgentQuery {
    /// Fallback for clients that cannot set headers on the upgrade request.
    #[serde(default)]
    pub token: String,
}

/// The upgrade is authenticated before it is accepted: an unauthenticated
/// socket would let anyone hold a connection slot.
pub async fn agent_ws(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<AgentQuery>,
    ws: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let mut headers = headers;
    if !headers.contains_key("authorization") && !query.token.is_empty() {
        if let Ok(value) = format!("Bearer {}", query.token).parse() {
            headers.insert("authorization", value);
        }
    }
    let principal = authenticate(&state, &headers).await?;
    let device_id = principal.device_id;
    Ok(ws.on_upgrade(move |socket| serve_agent(state, device_id, socket)))
}

async fn serve_agent(state: AppState, device_id: String, socket: WebSocket) {
    let (mut sink, mut stream) = socket.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    let generation = state.generation.fetch_add(1, Ordering::Relaxed);

    {
        let mut agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
        // A reconnect replaces the older socket rather than being refused: the
        // old one is usually a half-dead connection the device already gave up on.
        agents.insert(
            device_id.clone(),
            AgentConn { tx: tx.clone(), lan_addrs: Vec::new(), share_port: 0, generation },
        );
    }
    let _ = tx.send(json!({"type": "welcome"}).to_string());

    // One task owns the sink, so pushes from other requests never interleave
    // with replies to this device's own messages.
    let writer = tokio::spawn(async move {
        while let Some(text) = rx.recv().await {
            if sink.send(Message::Text(text.into())).await.is_err() {
                break;
            }
        }
    });

    while let Some(Ok(message)) = stream.next().await {
        let text = match message {
            Message::Text(text) => text.to_string(),
            Message::Close(_) => break,
            _ => continue,
        };
        let Ok(value) = serde_json::from_str::<Value>(&text) else { continue };
        match value.get("type").and_then(Value::as_str).unwrap_or_default() {
            "hello" => {
                let addrs: Vec<String> = value
                    .get("lan_addrs")
                    .and_then(Value::as_array)
                    .map(|items| {
                        items
                            .iter()
                            .filter_map(Value::as_str)
                            // Comma is the column separator in devices.lan_addrs.
                            .filter(|s| !s.is_empty() && !s.contains(','))
                            .map(|s| s.to_string())
                            .collect()
                    })
                    .unwrap_or_default();
                let port = value.get("port").and_then(Value::as_u64).unwrap_or(0).min(65535) as u16;
                {
                    let mut agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
                    if let Some(agent) = agents.get_mut(&device_id) {
                        if agent.generation == generation {
                            agent.lan_addrs = addrs.clone();
                            agent.share_port = port;
                        }
                    }
                }
                let joined = addrs.join(",");
                let id = device_id.clone();
                state
                    .db
                    .call(move |conn| {
                        let _ = conn.execute(
                            "UPDATE devices SET last_seen = ?1, lan_addrs = ?2, share_port = ?3 \
                             WHERE id = ?4",
                            params![iso(now()), joined, port as i64, id],
                        );
                    })
                    .await;
                let _ = tx.send(json!({"type": "ready"}).to_string());
            }
            "ping" => {
                let id = device_id.clone();
                state
                    .db
                    .call(move |conn| {
                        let _ = conn.execute(
                            "UPDATE devices SET last_seen = ?1 WHERE id = ?2",
                            params![iso(now()), id],
                        );
                    })
                    .await;
                let _ = tx.send(json!({"type": "pong"}).to_string());
            }
            _ => {}
        }
    }

    {
        let mut agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
        // Only if a newer socket has not already taken this device's slot.
        if agents.get(&device_id).map(|a| a.generation) == Some(generation) {
            agents.remove(&device_id);
        }
    }
    writer.abort();
}

#[derive(Deserialize)]
pub struct SessionRequest {
    #[serde(default)]
    pub device_id: String,
}

#[derive(Serialize)]
pub struct SessionResponse {
    pub session_id: String,
    pub ticket: String,
    pub expires_in: i64,
    pub peer_lan_addrs: Vec<String>,
    pub peer_port: u16,
}

/// Asks a peer device to accept one connection. The ticket is pushed to that
/// device over its agent socket, so its file share can check a presented ticket
/// against its own memory -- no round trip back here on every request.
pub async fn open_session(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(body): Json<SessionRequest>,
) -> Result<Json<SessionResponse>, ApiError> {
    let principal = authenticate(&state, &headers).await?;
    let target = body.device_id.trim().to_string();
    if target.is_empty() || target == principal.device_id {
        return Err(fail(StatusCode::BAD_REQUEST, "no target device"));
    }

    let user_id = principal.user_id;
    let from_device = principal.device_id.clone();
    let lookup = target.clone();
    let from_name = state
        .db
        .call(move |conn| {
            let owned = conn
                .query_row(
                    "SELECT 1 FROM devices WHERE id = ?1 AND user_id = ?2",
                    params![lookup, user_id],
                    |_| Ok(()),
                )
                .optional()
                .unwrap_or(None)
                .is_some();
            if !owned {
                return Err(fail(StatusCode::NOT_FOUND, "no such device"));
            }
            Ok(conn
                .query_row("SELECT name FROM devices WHERE id = ?1", params![from_device], |r| {
                    r.get::<_, String>(0)
                })
                .optional()
                .unwrap_or(None)
                .unwrap_or_default())
        })
        .await?;

    let session_id = random_hex(16);
    let ticket = random_token();

    let (lan_addrs, port) = {
        let agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
        let agent = agents
            .get(&target)
            .ok_or_else(|| fail(StatusCode::CONFLICT, "device is offline"))?;
        let notice = json!({
            "type": "incoming",
            "session_id": session_id,
            "ticket": ticket,
            "from": from_name,
            "from_device": principal.device_id,
            "expires_in": TICKET_TTL_SECONDS,
        });
        agent
            .tx
            .send(notice.to_string())
            .map_err(|_| fail(StatusCode::CONFLICT, "device is offline"))?;
        (agent.lan_addrs.clone(), agent.share_port)
    };

    Ok(Json(SessionResponse {
        session_id,
        ticket,
        expires_in: TICKET_TTL_SECONDS,
        peer_lan_addrs: lan_addrs,
        peer_port: port,
    }))
}
