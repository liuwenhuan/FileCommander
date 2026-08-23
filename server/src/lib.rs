//! FileCommander account server: accounts, device registry and the signalling
//! a transfer between two of the account's devices needs.
//!
//! Deliberately small -- SQLite, no ORM, no JWT library -- but it does terminate
//! its own TLS and rate-limit its own sign-in endpoints, because there is no
//! reverse proxy in front of it.

pub mod agent;
pub mod api;
pub mod clipboard;
pub mod db;
pub mod relay;

use std::collections::HashMap;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, AtomicUsize};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use axum::extract::ws::WebSocket;
use axum::extract::DefaultBodyLimit;
use axum::routing::{delete, get, post};
use axum::Router;
use tokio::sync::{mpsc, oneshot};

use db::Db;

/// One device's live agent WebSocket, as far as the rest of the server cares:
/// somewhere to push a connection request, plus what the device last told us
/// about how to reach it directly.
pub struct AgentConn {
    pub tx: mpsc::UnboundedSender<String>,
    /// Which account the socket belongs to, so a presence change can be pushed
    /// to that account's other devices and only them.
    pub user_id: i64,
    pub lan_addrs: Vec<String>,
    pub share_port: u16,
    /// The device's TLS pin, in curl's `sha256//<base64>` form. Handed to the
    /// peer so it can pin the self-signed certificate the file share serves.
    pub share_pin: String,
    /// True only after this socket's hello declared clipboard support.
    pub clipboard: bool,
    /// Distinguishes two connections from the same device, so a socket that
    /// closes after a newer one registered does not evict the newer one.
    pub generation: u64,
}

/// A relay session, live from the moment /v1/session mints it until a ticket
/// lifetime after its devices stop connecting. The queues hold sockets that
/// arrived before their opposite number; each entry is the waiting task, which
/// does the pumping once it is handed a peer.
pub struct RelaySession {
    pub ticket: String,
    pub expires: Instant,
    pub accepting: VecDeque<oneshot::Sender<WebSocket>>,
    pub connecting: VecDeque<oneshot::Sender<WebSocket>>,
}

#[derive(Clone)]
pub struct AppState {
    pub db: Db,
    pub agents: Arc<Mutex<HashMap<String, AgentConn>>>,
    /// Sessions the relay will join two sockets for, keyed by session id.
    pub sessions: Arc<Mutex<HashMap<String, RelaySession>>>,
    /// Sign-in attempts per client address, for the built-in rate limit.
    pub attempts: Arc<Mutex<HashMap<String, (u32, Instant)>>>,
    pub generation: Arc<AtomicU64>,
    /// Relay WebSockets currently open (paired or parked). Bounded by
    /// relay_max_conns so a flood of connections cannot exhaust the server.
    pub relay_conns: Arc<AtomicUsize>,
    /// Ceiling on relay_conns; a connection beyond it is refused.
    pub relay_max_conns: usize,
    /// How long an active relayed connection may sit with no byte moving
    /// before the relay reclaims it (see relay.rs).
    pub relay_idle_seconds: u64,
}

impl AppState {
    pub fn new(db: Db) -> AppState {
        AppState {
            db,
            agents: Arc::new(Mutex::new(HashMap::new())),
            sessions: Arc::new(Mutex::new(HashMap::new())),
            attempts: Arc::new(Mutex::new(HashMap::new())),
            generation: Arc::new(AtomicU64::new(1)),
            relay_conns: Arc::new(AtomicUsize::new(0)),
            relay_max_conns: 64,
            relay_idle_seconds: 300,
        }
    }
}

pub fn router(state: AppState) -> Router {
    // Only the credential endpoints are rate limited: an authenticated caller
    // already cost an attacker a valid token, and throttling /v1/devices would
    // punish a device that reconnects a lot on a flaky link.
    let auth = Router::new()
        .route("/v1/auth/register", post(api::register))
        .route("/v1/auth/login", post(api::login))
        .route("/v1/auth/refresh", post(api::refresh))
        .layer(axum::middleware::from_fn_with_state(
            state.clone(),
            api::rate_limit,
        ));

    let clipboard = Router::new()
        .route(
            "/v1/clipboard",
            get(clipboard::list)
                .post(clipboard::publish)
                .delete(clipboard::clear),
        )
        .route("/v1/clipboard/{item_id}", delete(clipboard::delete_item))
        .route(
            "/v1/clipboard/{item_id}/thumbnail",
            get(clipboard::thumbnail),
        )
        // Clipboard publishes include a base64 preview; reject oversized bodies
        // before JSON deserialization or allocation.
        .layer(DefaultBodyLimit::max(256 * 1024));

    let clipboard_delivery = Router::new()
        .route("/v1/clipboard/send", post(clipboard::send_delivery))
        .route("/v1/clipboard/deliveries", get(clipboard::deliveries))
        .route(
            "/v1/clipboard/deliveries/{delivery_id}/content",
            get(clipboard::delivery_content),
        )
        .route(
            "/v1/clipboard/deliveries/{delivery_id}/ack",
            post(clipboard::acknowledge_delivery),
        )
        .layer(DefaultBodyLimit::max(64 * 1024));

    Router::new()
        .merge(auth)
        .merge(clipboard)
        .merge(clipboard_delivery)
        .route("/v1/auth/logout", post(api::logout))
        .route("/v1/devices", get(api::devices))
        .route("/v1/devices/{device_id}", delete(api::forget_device))
        .route("/v1/agent", get(agent::agent_ws))
        .route("/v1/session", post(agent::open_session))
        .route("/v1/relay/{session_id}", get(relay::relay_ws))
        .with_state(state)
}
