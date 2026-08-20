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

use std::time::{Duration, Instant};

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Path, Query, State};
use axum::http::StatusCode;
use axum::response::Response;
use futures_util::stream::SplitSink;
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use tokio::sync::oneshot;

use crate::agent::TICKET_TTL_SECONDS;
use crate::api::{fail, ApiError};
use crate::AppState;

/// How long a parked socket waits for its peer before it is sent away. The
/// client parks a replacement immediately, and that reconnect is also what
/// keeps the session alive while the peer sits idle.
const PARK_SECONDS: u64 = 60;

#[derive(Deserialize)]
pub struct RelayQuery {
    #[serde(default)]
    pub ticket: String,
    /// `accept` for the device being browsed, `connect` for the one browsing.
    #[serde(default)]
    pub role: String,
}

pub async fn relay_ws(
    State(state): State<AppState>,
    Path(session_id): Path<String>,
    Query(query): Query<RelayQuery>,
    ws: WebSocketUpgrade,
) -> Result<Response, ApiError> {
    let accept = match query.role.as_str() {
        "accept" => true,
        "connect" => false,
        _ => return Err(fail(StatusCode::BAD_REQUEST, "unknown role")),
    };
    {
        let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        sessions.retain(|_, s| s.expires > now);
        let session = sessions
            .get_mut(&session_id)
            .ok_or_else(|| fail(StatusCode::FORBIDDEN, "no such session"))?;
        if session.ticket != query.ticket {
            return Err(fail(StatusCode::FORBIDDEN, "bad ticket"));
        }
        // Sliding expiry: a session lives as long as its devices keep using it,
        // and dies a ticket's lifetime after the last one stops. A fixed expiry
        // would cut a long browse off mid-transfer.
        session.expires = now + Duration::from_secs(TICKET_TTL_SECONDS as u64);
    }
    Ok(ws.on_upgrade(move |socket| join(state, session_id, accept, socket)))
}

/// Pairs `socket` with the session's other half: takes a socket already parked
/// by the opposite role, or parks this one and waits.
async fn join(state: AppState, session_id: String, accept: bool, socket: WebSocket) {
    let mut socket = socket;
    loop {
        let waiting = {
            let mut sessions = state.sessions.lock().unwrap_or_else(|e| e.into_inner());
            let Some(session) = sessions.get_mut(&session_id) else {
                return;
            };
            let queue = if accept { &mut session.connecting } else { &mut session.accepting };
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
        pump(socket, peer).await;
    }
}

/// Copies binary frames between the two sockets until either goes away.
async fn pump(a: WebSocket, b: WebSocket) {
    let (mut a_sink, mut a_stream) = a.split();
    let (mut b_sink, mut b_stream) = b.split();
    // Both sides are told, because the accepting one only opens its local
    // connection once it knows the socket is carrying something.
    let paired = Message::Text("{\"type\":\"paired\"}".into());
    let _ = a_sink.send(paired.clone()).await;
    let _ = b_sink.send(paired).await;

    loop {
        let alive = tokio::select! {
            msg = a_stream.next() => forward(msg, &mut b_sink).await,
            msg = b_stream.next() => forward(msg, &mut a_sink).await,
        };
        if !alive {
            break;
        }
    }
    let _ = a_sink.close().await;
    let _ = b_sink.close().await;
}

type Incoming = Option<Result<Message, axum::Error>>;

async fn forward(msg: Incoming, out: &mut SplitSink<WebSocket, Message>) -> bool {
    match msg {
        Some(Ok(Message::Binary(bytes))) => out.send(Message::Binary(bytes)).await.is_ok(),
        // Text frames are control, and there is no control to relay; ping/pong
        // are the socket's own business.
        Some(Ok(Message::Ping(_) | Message::Pong(_) | Message::Text(_))) => true,
        _ => false,
    }
}

