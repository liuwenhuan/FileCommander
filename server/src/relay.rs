//! The relay: a byte pipe between the two devices of one session.
//!
//! Only used when the devices cannot reach each other directly. It knows
//! nothing about WebDAV or about what it is carrying -- one WebSocket carries
//! one TCP connection, and the server's whole job is to join the two sockets
//! that quote the same session id.
//!
//! One socket per connection rather than a multiplexed stream because the
//! accessing side opens several connections in parallel (CurlWebDavProvider
//! reports four read channels), and a frame format of our own would be a
//! protocol to maintain for no gain. The serving side therefore parks a small
//! pool of sockets here, so a connection its peer makes finds one waiting.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Response;
use futures_util::stream::SplitSink;
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use tokio::sync::oneshot;

use crate::api::{fail, ApiError};
use crate::AppState;

/// How long a parked socket waits for its peer before it is sent away. The
/// client parks a replacement immediately, and that reconnect is also what
/// keeps the session alive while the peer sits idle.
const PARK_SECONDS: u64 = 60;
const END_MESSAGE: &str = "{\"type\":\"eof\"}";

#[derive(Deserialize)]
pub struct RelayQuery {
    /// `accept` for the device being browsed, `connect` for the one browsing.
    /// Not a secret, so it may stay in the URL query; the ticket does not.
    #[serde(default)]
    pub role: String,
}

/// One reserved slot in the relay's connection budget, released when the
/// connection finishes (paired, parked-away, or dropped).
struct RelaySlot(Arc<AtomicUsize>);

impl RelaySlot {
    fn acquire(counter: &Arc<AtomicUsize>, limit: usize) -> Option<Self> {
        let current = counter.fetch_add(1, Ordering::Relaxed);
        if current >= limit {
            counter.fetch_sub(1, Ordering::Relaxed);
            return None;
        }
        Some(RelaySlot(counter.clone()))
    }
}

impl Drop for RelaySlot {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::Relaxed);
    }
}

pub async fn relay_ws(
    State(state): State<AppState>,
    Path(session_id): Path<String>,
    headers: HeaderMap,
    Query(query): Query<RelayQuery>,
    ws: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let accept = match query.role.as_str() {
        "accept" => true,
        "connect" => false,
        _ => return Err(fail(StatusCode::BAD_REQUEST, "unknown role")),
    };
    // The ticket is the whole authority for a relay connection, and it must not
    // ride in the URL query where server/proxy access logs would record it.
    let ticket = headers
        .get("authorization")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
        .ok_or_else(|| fail(StatusCode::UNAUTHORIZED, "missing ticket"))?;
    {
        let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        sessions.retain(|_, s| s.expires > now);
        let session = sessions
            .get_mut(&session_id)
            .ok_or_else(|| fail(StatusCode::FORBIDDEN, "no such session"))?;
        if session.ticket != ticket {
            return Err(fail(StatusCode::FORBIDDEN, "bad ticket"));
        }
        // Absolute expiry: an established pipe may finish, but the bearer ticket
        // can never be refreshed into a permanent credential by reconnecting.
    }
    // Cap the number of relay WebSockets open at once, whatever they are doing.
    let slot = RelaySlot::acquire(&state.relay_conns, state.relay_max_conns)
        .ok_or_else(|| fail(StatusCode::SERVICE_UNAVAILABLE, "relay is busy"))?;
    Ok(ws.on_upgrade(move |socket| join(state, session_id, accept, socket, slot)))
}

/// Pairs `socket` with the session's other half: takes a socket already parked
/// by the opposite role, or parks this one and waits. `slot` is the connection
/// budget reservation made in relay_ws; holding it for the body of this task
/// releases it exactly when the socket stops existing.
async fn join(
    state: AppState,
    session_id: String,
    accept: bool,
    socket: WebSocket,
    _slot: RelaySlot,
) {
    let mut socket = socket;
    let idle_secs = state.relay_idle_seconds;
    loop {
        let waiting = {
            let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
            let Some(session) = sessions.get_mut(&session_id) else {
                return;
            };
            let queue = if accept {
                &mut session.connecting
            } else {
                &mut session.accepting
            };
            queue.pop_front()
        };
        match waiting {
            // A sender whose task has already given up hands the socket back,
            // so keep popping rather than pairing with nobody.
            Some(tx) => match tx.send(socket) {
                Ok(()) => return,
                Err(returned) => socket = returned,
            },
            None => break,
        }
    }

    let (tx, rx) = oneshot::channel();
    {
        let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
        let Some(session) = sessions.get_mut(&session_id) else {
            return;
        };
        if accept {
            session.accepting.push_back(tx);
        } else {
            session.connecting.push_back(tx);
        }
    }
    if let Ok(Ok(peer)) = tokio::time::timeout(Duration::from_secs(PARK_SECONDS), rx).await {
        pump(socket, peer, idle_secs).await;
    }
}

/// Copies binary frames between the two sockets until either goes away, or
/// until `idle_secs` pass with no frame at all. The idle bound exists for the
/// partition case: when neither side closes but packets simply stop flowing,
/// the sockets would otherwise be held forever. curl already bounds a hung
/// *request* on the accessing side; this is the backstop for a half-dead link
/// that never sends a FIN, and it is generous so a slow disk or a pause between
/// WebDAV requests cannot trip it.
async fn pump(a: WebSocket, b: WebSocket, idle_secs: u64) {
    let (mut a_sink, mut a_stream) = a.split();
    let (mut b_sink, mut b_stream) = b.split();
    // Both sides are told, because the accepting one only opens its local
    // connection once it knows the socket is carrying something.
    let paired = Message::Text("{\"type\":\"paired\"}".into());
    let _ = a_sink.send(paired.clone()).await;
    let _ = b_sink.send(paired).await;

    let mut a_eof = false;
    let mut b_eof = false;
    while !a_eof || !b_eof {
        let result = tokio::time::timeout(Duration::from_secs(idle_secs), async {
            tokio::select! {
                msg = a_stream.next() => (true, forward(msg, &mut b_sink, a_eof).await),
                msg = b_stream.next() => (false, forward(msg, &mut a_sink, b_eof).await),
            }
        })
        .await;
        let (from_a, outcome) = match result {
            Ok(result) => result,
            Err(_) => break, // idle: reclaim a connection nobody is using
        };
        match outcome {
            ForwardResult::Continue => {}
            ForwardResult::Eof => {
                if from_a {
                    a_eof = true;
                } else {
                    b_eof = true;
                }
            }
            ForwardResult::Closed => break,
        }
    }
    let _ = a_sink.close().await;
    let _ = b_sink.close().await;
}

type Incoming = Option<Result<Message, axum::Error>>;

enum ForwardResult {
    Continue,
    Eof,
    Closed,
}

async fn forward(
    msg: Incoming,
    out: &mut SplitSink<WebSocket, Message>,
    eof: bool,
) -> ForwardResult {
    match msg {
        Some(Ok(Message::Binary(bytes))) if !eof => {
            if out.send(Message::Binary(bytes)).await.is_ok() {
                ForwardResult::Continue
            } else {
                ForwardResult::Closed
            }
        }
        // EOF closes only this input direction. Keep polling it for the physical
        // close, but reject payload after EOF while the peer drains its own tail.
        Some(Ok(Message::Text(text))) if text == END_MESSAGE => ForwardResult::Eof,
        Some(Ok(Message::Binary(_))) => ForwardResult::Closed,
        // Other text frames are control; ping/pong are the socket's own business.
        Some(Ok(Message::Ping(_) | Message::Pong(_) | Message::Text(_))) => ForwardResult::Continue,
        _ => ForwardResult::Closed,
    }
}
