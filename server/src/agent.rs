//! Presence and signalling.
//!
//! Each signed-in device keeps one WebSocket open here. It tells the server how
//! to reach it (LAN addresses, share port); the server pushes it a ticket when
//! another of the account's devices wants to connect. The bytes of a transfer
//! never pass through this socket -- the two devices talk WebDAV directly.

use std::collections::{HashMap, VecDeque};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::response::Response;
use axum::Json;
use futures_util::{SinkExt, StreamExt};
use rusqlite::{params, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tokio::sync::mpsc;

use crate::api::{authenticate, check_protocol, fail, log_line, ApiError};
use crate::db::{iso, now, random_hex, random_token};
use crate::{AgentConn, AppState, RelaySession};

/// Pushes a presence change to the account's other connected devices, so they
/// can refresh their device list immediately instead of waiting for a manual
/// fetch. The sender is skipped; the frame is idempotent, so a peer that gets
/// several just re-fetches.
fn broadcast_presence(
    agents: &HashMap<String, AgentConn>,
    user_id: i64,
    device_id: &str,
    online: bool,
) {
    let frame = json!({"type": "presence", "device_id": device_id, "online": online}).to_string();
    for (id, agent) in agents {
        if id != device_id && agent.user_id == user_id {
            let _ = agent.tx.send(frame.clone());
        }
    }
}

/// Clipboard updates contain no content. Capable peers re-fetch their own
/// account history; the publisher is deliberately excluded.
pub fn broadcast_clipboard(
    state: &AppState,
    user_id: i64,
    source_device_id: &str,
    revision: i64,
    change: &str,
) {
    let frame = json!({
        "type": "clipboard_changed",
        "revision": revision,
        "change": change,
    })
    .to_string();
    let agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
    for (device_id, agent) in agents.iter() {
        if device_id != source_device_id && agent.user_id == user_id && agent.clipboard {
            let _ = agent.tx.send(frame.clone());
        }
    }
}

/// One committed delivery and the specific device that may fetch it.
pub struct ClipboardDeliveryTarget {
    pub device_id: String,
    pub delivery_id: String,
}

/// Lightweight delivery data safe to push over an agent WebSocket.
pub struct ClipboardDeliveryMetadata {
    pub kind: String,
    pub size: i64,
}

/// Tells only online, account-owned recipients that their own pending delivery
/// is ready. The payload bytes stay in SQLite and must be fetched separately.
pub fn broadcast_clipboard_delivery(
    state: &AppState,
    user_id: i64,
    target_device_ids: &[ClipboardDeliveryTarget],
    delivery_metadata: &ClipboardDeliveryMetadata,
) {
    let agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
    for target in target_device_ids {
        if let Some(agent) = agents.get(&target.device_id) {
            if agent.user_id == user_id {
                let frame = json!({
                    "type": "clipboard_delivery",
                    "delivery_id": target.delivery_id,
                    "kind": delivery_metadata.kind,
                    "size": delivery_metadata.size,
                })
                .to_string();
                let _ = agent.tx.send(frame);
            }
        }
    }
}

/// How long a device has to use a ticket before the peer forgets it. Long
/// enough to survive a slow LAN probe, short enough that a leaked ticket is
/// worth little.
pub const TICKET_TTL_SECONDS: i64 = 300;

/// The upgrade is authenticated before it is accepted: an unauthenticated
/// socket would let anyone hold a connection slot.
///
/// The access token arrives only as an `Authorization: Bearer …` header. It is
/// deliberately NOT accepted from the URL query any more: a token in the query
/// string is written to server/proxy access logs, which a header is not.
pub async fn agent_ws(
    State(state): State<AppState>,
    headers: HeaderMap,
    ws: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    check_protocol(&headers)?;
    let principal = authenticate(&state, &headers).await?;
    let user_id = principal.user_id;
    let device_id = principal.device_id;
    log_line("agent", &format!("connect dev={device_id}"));
    Ok(ws.on_upgrade(move |socket| serve_agent(state, user_id, device_id, socket)))
}

async fn serve_agent(state: AppState, user_id: i64, device_id: String, socket: WebSocket) {
    let (mut sink, mut stream) = socket.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    let generation = state.generation.fetch_add(1, Ordering::Relaxed);

    {
        let mut agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
        // A reconnect replaces the older socket rather than being refused: the
        // old one is usually a half-dead connection the device already gave up on.
        agents.insert(
            device_id.clone(),
            AgentConn {
                tx: tx.clone(),
                user_id,
                lan_addrs: Vec::new(),
                share_port: 0,
                share_pin: String::new(),
                clipboard: false,
                generation,
            },
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
        let Ok(value) = serde_json::from_str::<Value>(&text) else {
            continue;
        };
        match value
            .get("type")
            .and_then(Value::as_str)
            .unwrap_or_default()
        {
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
                let port = value
                    .get("port")
                    .and_then(Value::as_u64)
                    .unwrap_or(0)
                    .min(65535) as u16;
                // The device's TLS pin, passed straight through to whoever asks
                // to connect to it. Only the shape is checked -- the server has
                // no way to know which key the device actually holds -- but a
                // value that is not a pin must never reach a peer's curl.
                let pin = value
                    .get("pin")
                    .and_then(Value::as_str)
                    .filter(|s| s.starts_with("sha256//") && s.len() <= 128 && !s.contains(','))
                    .unwrap_or_default()
                    .to_string();
                // The share names this device is actually serving, for the
                // device list to show before a peer browses it. Same
                // comma-separated convention as lan_addrs.
                let shares: Vec<String> = value
                    .get("shares")
                    .and_then(Value::as_array)
                    .map(|items| {
                        items
                            .iter()
                            .filter_map(Value::as_str)
                            .filter(|s| !s.is_empty() && !s.contains(','))
                            .map(|s| s.to_string())
                            .collect()
                    })
                    .unwrap_or_default();
                let clipboard = value
                    .get("capabilities")
                    .and_then(Value::as_array)
                    .is_some_and(|items| {
                        items.iter().any(|item| item.as_str() == Some("clipboard"))
                    });
                {
                    let mut agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
                    if let Some(agent) = agents.get_mut(&device_id) {
                        if agent.generation == generation {
                            agent.lan_addrs = addrs.clone();
                            agent.share_port = port;
                            agent.share_pin = pin.clone();
                            agent.clipboard = clipboard;
                        }
                    }
                }
                let joined = addrs.join(",");
                let shares_joined = shares.join(",");
                let id = device_id.clone();
                state
                    .db
                    .call(move |conn| {
                        let _ = conn.execute(
                            "UPDATE devices SET last_seen = ?1, lan_addrs = ?2, share_port = ?3, \
                             share_pin = ?4, shares = ?5 WHERE id = ?6",
                            params![iso(now()), joined, port as i64, pin, shares_joined, id],
                        );
                    })
                    .await;
                // last_seen is now fresh, so tell this device's peers it is
                // online: they re-fetch and see it, instead of waiting for a
                // manual poll.
                {
                    let agents = state.agents.lock().unwrap_or_else(|e| e.into_inner());
                    broadcast_presence(&agents, user_id, &device_id, true);
                }
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
            broadcast_presence(&agents, user_id, &device_id, false);
        }
    }
    log_line("agent", &format!("disconnect dev={device_id}"));
    writer.abort();
}

#[derive(Deserialize)]
pub struct SessionRequest {
    #[serde(default)]
    pub device_id: String,
    // Deserialize the retired field only to reject old peer-clipboard clients
    // explicitly rather than turning their scoped transfer into a folder share.
    #[serde(default, rename = "clipboard_item_id")]
    pub retired_clipboard_item_id: String,
}

#[derive(Serialize)]
pub struct SessionResponse {
    pub session_id: String,
    pub ticket: String,
    pub expires_in: i64,
    pub peer_lan_addrs: Vec<String>,
    pub peer_port: u16,
    /// The peer's TLS pin. Empty when the peer is running a build from before
    /// this existed, in which case the caller gets no confidentiality and knows it.
    pub peer_pin: String,
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
    if !body.retired_clipboard_item_id.trim().is_empty() {
        return Err(fail(
            StatusCode::UPGRADE_REQUIRED,
            "Your version of FileCommander is too old. Please update to continue.",
        ));
    }
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
                .query_row(
                    "SELECT name FROM devices WHERE id = ?1",
                    params![from_device],
                    |r| r.get::<_, String>(0),
                )
                .optional()
                .unwrap_or(None)
                .unwrap_or_default())
        })
        .await?;

    let session_id = random_hex(16);
    let ticket = random_token();

    let (lan_addrs, port, pin) = {
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
        (
            agent.lan_addrs.clone(),
            agent.share_port,
            agent.share_pin.clone(),
        )
    };

    // Recorded only once the target has actually been told: a session nobody
    // was notified about is one nothing will ever join.
    {
        let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        sessions.retain(|_, s| s.expires > now);
        sessions.insert(
            session_id.clone(),
            RelaySession {
                ticket: ticket.clone(),
                expires: now + Duration::from_secs(TICKET_TTL_SECONDS as u64),
                accepting: VecDeque::new(),
                connecting: VecDeque::new(),
            },
        );
    }

    Ok(Json(SessionResponse {
        session_id,
        ticket,
        expires_in: TICKET_TTL_SECONDS,
        peer_lan_addrs: lan_addrs,
        peer_port: port,
        peer_pin: pin,
    }))
}
