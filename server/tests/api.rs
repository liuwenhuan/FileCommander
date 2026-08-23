//! End-to-end tests for the account API.
//!
//! Most drive the router directly through `oneshot`, which needs no listener.
//! The agent/session pair needs a real socket, so those tests bind one.

use std::net::SocketAddr;
use std::time::Duration;

use axum::body::Body;
use axum::http::{Request, StatusCode};
use axum::response::Response;
use filecommander_account::{db::Db, router, AppState};
use http_body_util::BodyExt;
use serde_json::{json, Value};
use tower::ServiceExt;

fn state() -> AppState {
    AppState::new(Db::memory().expect("in-memory database"))
}

async fn send(state: &AppState, method: &str, path: &str, token: &str, body: Value) -> Response {
    let mut req = Request::builder()
        .method(method)
        .uri(path)
        .header("content-type", "application/json")
        .header("x-filecommander-protocol", "1");
    if !token.is_empty() {
        req = req.header("authorization", format!("Bearer {token}"));
    }
    let body = if body.is_null() {
        Body::empty()
    } else {
        Body::from(body.to_string())
    };
    router(state.clone())
        .oneshot(req.body(body).unwrap())
        .await
        .unwrap()
}

async fn send_raw(
    state: &AppState,
    path: &str,
    token: &str,
    content_type: &str,
    body: Vec<u8>,
) -> Response {
    router(state.clone())
        .oneshot(
            Request::builder()
                .method("POST")
                .uri(path)
                .header("content-type", content_type)
                .header("x-filecommander-protocol", "1")
                .header("authorization", format!("Bearer {token}"))
                .body(Body::from(body))
                .unwrap(),
        )
        .await
        .unwrap()
}

async fn send_text(state: &AppState, path: &str, token: &str, text: &str) -> Response {
    send_raw(
        state,
        path,
        token,
        "text/plain; charset=utf-8",
        text.as_bytes().to_vec(),
    )
    .await
}

async fn send_image(
    state: &AppState,
    token: &str,
    mime: &str,
    width: u64,
    height: u64,
    sha256: &str,
    body: Vec<u8>,
) -> Response {
    router(state.clone())
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/v1/clipboard/send")
                .header("content-type", mime)
                .header("x-filecommander-protocol", "1")
                .header("authorization", format!("Bearer {token}"))
                .header("x-clipboard-width", width)
                .header("x-clipboard-height", height)
                .header("x-clipboard-sha256", sha256)
                .body(Body::from(body))
                .unwrap(),
        )
        .await
        .unwrap()
}

async fn json_of(response: Response) -> Value {
    let bytes = response.into_body().collect().await.unwrap().to_bytes();
    serde_json::from_slice(&bytes).unwrap_or(Value::Null)
}

async fn register(state: &AppState, email: &str, password: &str) -> Response {
    send(
        state,
        "POST",
        "/v1/auth/register",
        "",
        json!({"email": email, "password": password}),
    )
    .await
}

async fn login(state: &AppState, email: &str, name: &str, device_id: &str) -> Value {
    let response = send(
        state,
        "POST",
        "/v1/auth/login",
        "",
        json!({
            "email": email,
            "password": "correct horse",
            "device_name": name,
            "platform": "linux",
            "device_id": device_id,
        }),
    )
    .await;
    assert_eq!(response.status(), StatusCode::OK);
    json_of(response).await
}

async fn publish_image(state: &AppState, token: &str, source_device_id: &str) -> Value {
    let response = send(
        state,
        "POST",
        "/v1/clipboard",
        token,
        json!({
            "type": "image",
            "thumbnail_base64": "aGVsbG8=",
            "thumbnail_mime": "image/png",
            "mime": "image/jpeg",
            "size": 123,
            "width": 10,
            "height": 20,
            "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "source_device_id": source_device_id,
        }),
    )
    .await;
    assert_eq!(response.status(), StatusCode::OK);
    json_of(response).await
}

#[tokio::test]
async fn register_rejects_bad_input_and_duplicates() {
    let state = state();
    assert_eq!(
        register(&state, "user@example.com", "correct horse")
            .await
            .status(),
        StatusCode::CREATED
    );

    let duplicate = register(&state, "USER@example.com", "correct horse").await;
    assert_eq!(duplicate.status(), StatusCode::CONFLICT);
    assert_eq!(
        json_of(duplicate).await["detail"],
        "email already registered"
    );

    assert_eq!(
        register(&state, "nope", "correct horse").await.status(),
        StatusCode::BAD_REQUEST
    );
    assert_eq!(
        register(&state, "a@b.c", "short").await.status(),
        StatusCode::BAD_REQUEST
    );
}

#[tokio::test]
async fn login_hides_whether_the_account_exists() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;

    let wrong = send(
        &state,
        "POST",
        "/v1/auth/login",
        "",
        json!({"email": "user@example.com", "password": "wrong password"}),
    )
    .await;
    let missing = send(
        &state,
        "POST",
        "/v1/auth/login",
        "",
        json!({"email": "ghost@example.com", "password": "correct horse"}),
    )
    .await;
    assert_eq!(wrong.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(missing.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(
        json_of(wrong).await["detail"],
        json_of(missing).await["detail"]
    );
}

#[tokio::test]
async fn login_reclaims_the_device_id_it_is_given() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;

    let first = login(&state, "user@example.com", "desktop", "").await;
    let device_id = first["device_id"].as_str().unwrap().to_string();
    assert!(!first["access_token"].as_str().unwrap().is_empty());
    assert_eq!(first["expires_in"], 900);

    let again = login(&state, "user@example.com", "desktop", &device_id).await;
    assert_eq!(again["device_id"], device_id.as_str());

    let fresh = login(&state, "user@example.com", "laptop", "").await;
    assert_ne!(fresh["device_id"], device_id.as_str());

    // A device id belonging to nobody must not be claimable.
    let stolen = login(
        &state,
        "user@example.com",
        "laptop",
        "ffffffffffffffffffffffffffffffff",
    )
    .await;
    assert_ne!(stolen["device_id"], "ffffffffffffffffffffffffffffffff");
}

#[tokio::test]
async fn refresh_rotates_and_spends_the_old_token() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    let session = login(&state, "user@example.com", "desktop", "").await;
    let refresh_token = session["refresh_token"].as_str().unwrap().to_string();

    let renewed = send(
        &state,
        "POST",
        "/v1/auth/refresh",
        "",
        json!({"refresh_token": refresh_token}),
    )
    .await;
    assert_eq!(renewed.status(), StatusCode::OK);
    let renewed = json_of(renewed).await;
    assert_eq!(renewed["device_id"], session["device_id"]);
    assert_ne!(renewed["refresh_token"], refresh_token.as_str());

    // The spent token, and the access token it replaced, are both dead now.
    let replayed = send(
        &state,
        "POST",
        "/v1/auth/refresh",
        "",
        json!({"refresh_token": refresh_token}),
    )
    .await;
    assert_eq!(replayed.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(json_of(replayed).await["detail"], "invalid refresh token");

    let old_access = session["access_token"].as_str().unwrap();
    assert_eq!(
        send(&state, "GET", "/v1/devices", old_access, Value::Null)
            .await
            .status(),
        StatusCode::UNAUTHORIZED
    );
}

#[tokio::test]
async fn devices_lists_the_account_and_marks_this_one() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    let desktop = login(&state, "user@example.com", "desktop", "").await;
    let laptop = login(&state, "user@example.com", "laptop", "").await;
    let token = laptop["access_token"].as_str().unwrap();

    let response = send(&state, "GET", "/v1/devices", token, Value::Null).await;
    assert_eq!(response.status(), StatusCode::OK);
    let body = json_of(response).await;
    // The Qt client rejects anything that is not a bare array.
    let list = body.as_array().expect("a JSON array");
    assert_eq!(list.len(), 2);
    assert_eq!(list[0]["name"], "desktop");
    assert_eq!(list[0]["self"], false);
    assert_eq!(list[1]["self"], true);
    // Nothing has been seen by the agent socket yet.
    assert_eq!(list[0]["online"], false);
    assert!(list[0]["lan_addrs"].as_array().unwrap().is_empty());

    let device_id = desktop["device_id"].as_str().unwrap();
    let path = format!("/v1/devices/{device_id}");
    assert_eq!(
        send(&state, "DELETE", &path, token, Value::Null)
            .await
            .status(),
        StatusCode::NO_CONTENT
    );
    let gone = send(&state, "DELETE", &path, token, Value::Null).await;
    assert_eq!(gone.status(), StatusCode::NOT_FOUND);
    assert_eq!(json_of(gone).await["detail"], "no such device");

    // The removed device's session went with it.
    assert_eq!(
        send(
            &state,
            "GET",
            "/v1/devices",
            desktop["access_token"].as_str().unwrap(),
            Value::Null
        )
        .await
        .status(),
        StatusCode::UNAUTHORIZED
    );
}

#[tokio::test]
async fn authentication_is_required_and_logout_ends_the_session() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    let session = login(&state, "user@example.com", "desktop", "").await;
    let token = session["access_token"].as_str().unwrap().to_string();

    let anonymous = send(&state, "GET", "/v1/devices", "", Value::Null).await;
    assert_eq!(anonymous.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(json_of(anonymous).await["detail"], "missing bearer token");

    let bogus = send(&state, "GET", "/v1/devices", "not-a-token", Value::Null).await;
    assert_eq!(bogus.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(json_of(bogus).await["detail"], "invalid or expired token");

    assert_eq!(
        send(&state, "POST", "/v1/auth/logout", &token, Value::Null)
            .await
            .status(),
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        send(&state, "GET", "/v1/devices", &token, Value::Null)
            .await
            .status(),
        StatusCode::UNAUTHORIZED
    );
}

#[tokio::test]
async fn sign_in_attempts_are_rate_limited() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    let mut last = StatusCode::OK;
    for _ in 0..25 {
        last = send(
            &state,
            "POST",
            "/v1/auth/login",
            "",
            json!({"email": "user@example.com", "password": "wrong password"}),
        )
        .await
        .status();
    }
    assert_eq!(last, StatusCode::TOO_MANY_REQUESTS);

    // Only the credential endpoints are throttled.
    assert_eq!(
        send(&state, "GET", "/v1/devices", "", Value::Null)
            .await
            .status(),
        StatusCode::UNAUTHORIZED
    );
}

/// Serves `state` on a real loopback port, for the tests that need a WebSocket.
async fn serve(state: AppState) -> SocketAddr {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let app = router(state).into_make_service_with_connect_info::<SocketAddr>();
    tokio::spawn(async move {
        axum::serve(listener, app).await.unwrap();
    });
    addr
}

/// Builds a WebSocket handshake request with a bearer credential in the
/// `Authorization` header -- the way the client now sends it, so the secret
/// never appears in the URL. Built from the URL string so tungstenite fills in
/// the handshake headers (Sec-WebSocket-Key, Upgrade) before the auth header
/// is added.
fn ws_request(addr: SocketAddr, path: &str, bearer: &str) -> Request<()> {
    use tokio_tungstenite::tungstenite::client::IntoClientRequest;
    let mut request: Request<()> = format!("ws://{addr}{path}")
        .into_client_request()
        .expect("valid ws url");
    request
        .headers_mut()
        .insert("authorization", format!("Bearer {bearer}").parse().unwrap());
    request
        .headers_mut()
        .insert("x-filecommander-protocol", "1".parse().unwrap());
    request
}

#[tokio::test]
async fn an_agent_reports_presence_and_receives_a_ticket() {
    use futures_util::{SinkExt, StreamExt};

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "user@example.com", "correct horse").await;
    let host = login(&state, "user@example.com", "desktop", "").await;
    let guest = login(&state, "user@example.com", "laptop", "").await;
    let host_id = host["device_id"].as_str().unwrap().to_string();

    let (mut socket, _) = tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/agent",
        host["access_token"].as_str().unwrap(),
    ))
    .await
    .unwrap();
    assert!(socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("welcome"));

    socket
        .send(tokio_tungstenite::tungstenite::Message::Text(
            json!({"type": "hello", "lan_addrs": ["192.168.1.7"], "port": 45001})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
    assert!(socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("ready"));

    // Presence reached the device row, so the other device sees it online.
    let listed = json_of(
        send(
            &state,
            "GET",
            "/v1/devices",
            guest["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    let seen = listed
        .as_array()
        .unwrap()
        .iter()
        .find(|d| d["id"] == host_id.as_str())
        .unwrap();
    assert_eq!(seen["online"], true);
    assert_eq!(seen["lan_addrs"][0], "192.168.1.7");

    let opened = send(
        &state,
        "POST",
        "/v1/session",
        guest["access_token"].as_str().unwrap(),
        json!({"device_id": host_id}),
    )
    .await;
    assert_eq!(opened.status(), StatusCode::OK);
    let opened = json_of(opened).await;
    assert_eq!(opened["peer_port"], 45001);
    assert_eq!(opened["peer_lan_addrs"][0], "192.168.1.7");

    // The same ticket the caller got was pushed to the device being asked for.
    let pushed: Value =
        serde_json::from_str(socket.next().await.unwrap().unwrap().to_text().unwrap()).unwrap();
    assert_eq!(pushed["type"], "incoming");
    assert_eq!(pushed["ticket"], opened["ticket"]);
    assert_eq!(pushed["from"], "laptop");
}

#[tokio::test]
async fn a_session_needs_an_owned_device_that_is_online() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    register(&state, "other@example.com", "correct horse").await;
    let mine = login(&state, "user@example.com", "desktop", "").await;
    let theirs = login(&state, "other@example.com", "their box", "").await;
    let token = mine["access_token"].as_str().unwrap();

    let foreign = send(
        &state,
        "POST",
        "/v1/session",
        token,
        json!({"device_id": theirs["device_id"]}),
    )
    .await;
    assert_eq!(foreign.status(), StatusCode::NOT_FOUND);

    let laptop = login(&state, "user@example.com", "laptop", "").await;
    let offline = send(
        &state,
        "POST",
        "/v1/session",
        token,
        json!({"device_id": laptop["device_id"]}),
    )
    .await;
    assert_eq!(offline.status(), StatusCode::CONFLICT);
    assert_eq!(json_of(offline).await["detail"], "device is offline");
}

/// Opens a session between two devices, returning (session_id, ticket) and the
/// agent socket, which has to stay alive for the session to be reachable.
async fn open_relay_session(state: &AppState, addr: SocketAddr) -> (String, String) {
    use futures_util::{SinkExt, StreamExt};

    register(state, "user@example.com", "correct horse").await;
    let host = login(state, "user@example.com", "desktop", "").await;
    let guest = login(state, "user@example.com", "laptop", "").await;

    let (mut agent, _) = tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/agent",
        host["access_token"].as_str().unwrap(),
    ))
    .await
    .unwrap();
    agent.next().await; // welcome
    agent
        .send(tokio_tungstenite::tungstenite::Message::Text(
            json!({"type": "hello", "lan_addrs": [], "port": 45001})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
    agent.next().await; // ready

    let opened = json_of(
        send(
            state,
            "POST",
            "/v1/session",
            guest["access_token"].as_str().unwrap(),
            json!({"device_id": host["device_id"]}),
        )
        .await,
    )
    .await;
    (
        opened["session_id"].as_str().unwrap().to_string(),
        opened["ticket"].as_str().unwrap().to_string(),
    )
}

#[tokio::test]
async fn the_relay_joins_the_two_sockets_of_one_session() {
    use futures_util::{SinkExt, StreamExt};
    use tokio_tungstenite::tungstenite::Message;

    let state = state();
    let addr = serve(state.clone()).await;
    let (session_id, ticket) = open_relay_session(&state, addr).await;

    // The serving side parks first, exactly as the client's tunnel does. The
    // ticket rides in the Authorization header; only the role is in the URL.
    let park = format!("/v1/relay/{session_id}?role=accept");
    let (mut accepting, _) = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();
    let dial = format!("/v1/relay/{session_id}?role=connect");
    let (mut connecting, _) = tokio_tungstenite::connect_async(ws_request(addr, &dial, &ticket))
        .await
        .unwrap();

    // Both learn they are paired before any payload moves.
    assert!(accepting
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("paired"));
    assert!(connecting
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("paired"));

    connecting
        .send(Message::Binary(b"PROPFIND / HTTP/1.1".to_vec().into()))
        .await
        .unwrap();
    let seen = accepting.next().await.unwrap().unwrap();
    assert_eq!(seen.into_data().as_ref(), b"PROPFIND / HTTP/1.1");

    accepting
        .send(Message::Binary(b"HTTP/1.1 207".to_vec().into()))
        .await
        .unwrap();
    let back = connecting.next().await.unwrap().unwrap();
    assert_eq!(back.into_data().as_ref(), b"HTTP/1.1 207");

    // Either side closing takes the pipe down, so the peer is not left hanging.
    drop(connecting);
    assert!(accepting
        .next()
        .await
        .map(|m| m.is_err() || m.unwrap().is_close())
        .unwrap_or(true));
}

#[tokio::test]
async fn the_relay_delivers_the_last_frame_before_eof() {
    use futures_util::{SinkExt, StreamExt};
    use tokio_tungstenite::tungstenite::Message;

    let state = state();
    let addr = serve(state.clone()).await;
    let (session_id, ticket) = open_relay_session(&state, addr).await;

    let park = format!("/v1/relay/{session_id}?role=accept");
    let (mut accepting, _) = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();
    let dial = format!("/v1/relay/{session_id}?role=connect");
    let (mut connecting, _) = tokio_tungstenite::connect_async(ws_request(addr, &dial, &ticket))
        .await
        .unwrap();
    accepting.next().await;
    connecting.next().await;

    connecting
        .send(Message::Binary(b"request tail".to_vec().into()))
        .await
        .unwrap();
    connecting
        .send(Message::Text("{\"type\":\"eof\"}".into()))
        .await
        .unwrap();

    let last = tokio::time::timeout(Duration::from_secs(2), accepting.next())
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    assert_eq!(last.into_data().as_ref(), b"request tail");

    // One direction ending must not close the pipe before the peer sends its tail.
    assert!(
        tokio::time::timeout(Duration::from_millis(100), accepting.next())
            .await
            .is_err()
    );
    accepting
        .send(Message::Binary(b"response tail".to_vec().into()))
        .await
        .unwrap();
    accepting
        .send(Message::Text("{\"type\":\"eof\"}".into()))
        .await
        .unwrap();

    let last = tokio::time::timeout(Duration::from_secs(2), connecting.next())
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    assert_eq!(last.into_data().as_ref(), b"response tail");
    let closed = tokio::time::timeout(Duration::from_secs(2), connecting.next())
        .await
        .unwrap();
    assert!(closed
        .map(|m| m.is_err() || m.unwrap().is_close())
        .unwrap_or(true));

    // After EOF the stream is still polled for its physical fallback close.
    let (mut accepting, _) = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();
    let (mut connecting, _) = tokio_tungstenite::connect_async(ws_request(addr, &dial, &ticket))
        .await
        .unwrap();
    tokio::time::timeout(Duration::from_secs(2), accepting.next())
        .await
        .unwrap();
    tokio::time::timeout(Duration::from_secs(2), connecting.next())
        .await
        .unwrap();
    connecting
        .send(Message::Text("{\"type\":\"eof\"}".into()))
        .await
        .unwrap();
    drop(connecting);
    let closed = tokio::time::timeout(Duration::from_secs(2), accepting.next())
        .await
        .unwrap();
    assert!(closed
        .map(|m| m.is_err() || m.unwrap().is_close())
        .unwrap_or(true));
}

#[tokio::test]
async fn the_relay_refuses_a_wrong_ticket_or_an_unknown_session() {
    let state = state();
    let addr = serve(state.clone()).await;
    let (session_id, ticket) = open_relay_session(&state, addr).await;

    // Wrong ticket, unknown session, bad role -- each refused before the upgrade.
    assert!(tokio_tungstenite::connect_async(ws_request(
        addr,
        &format!("/v1/relay/{session_id}?role=accept"),
        "wrong",
    ))
    .await
    .is_err());
    assert!(tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/relay/deadbeef?role=accept",
        &ticket,
    ))
    .await
    .is_err());
    assert!(tokio_tungstenite::connect_async(ws_request(
        addr,
        &format!("/v1/relay/{session_id}?role=sideways"),
        &ticket,
    ))
    .await
    .is_err());
}

/// The agent no longer accepts its token from the URL query: only an
/// Authorization header authenticates the upgrade, because a query token is
/// written to access logs and a header is not.
#[tokio::test]
async fn the_agent_takes_the_token_from_the_header_not_the_query() {
    use futures_util::StreamExt;

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "user@example.com", "correct horse").await;
    let host = login(&state, "user@example.com", "desktop", "").await;
    let token = host["access_token"].as_str().unwrap();

    // A token in the query, with no header, is refused: the URL is the thing
    // that gets logged, so it must not carry the credential.
    let leaked = format!("ws://{addr}/v1/agent?token={token}");
    assert!(tokio_tungstenite::connect_async(leaked).await.is_err());

    // The same token in a header succeeds.
    let (mut socket, _) = tokio_tungstenite::connect_async(ws_request(addr, "/v1/agent", token))
        .await
        .unwrap();
    assert!(socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("welcome"));
}

/// The relay refuses to grow beyond its connection budget, so a flood of
/// sockets cannot exhaust the server.
#[tokio::test]
async fn the_relay_caps_concurrent_connections() {
    let mut state = state();
    state.relay_max_conns = 2;
    let addr = serve(state.clone()).await;
    let (session_id, ticket) = open_relay_session(&state, addr).await;

    let park = format!("/v1/relay/{session_id}?role=accept");
    let _a = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();
    let _b = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();

    // The third exceeds the cap and is refused before the upgrade.
    assert!(
        tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
            .await
            .is_err()
    );
}

/// An active relayed connection that goes quiet is reclaimed after the idle
/// bound, so a half-dead link (no FIN, no bytes) cannot be held forever.
#[tokio::test]
async fn an_idle_relay_connection_is_reclaimed() {
    use futures_util::StreamExt;

    let mut state = state();
    state.relay_idle_seconds = 1;
    let addr = serve(state.clone()).await;
    let (session_id, ticket) = open_relay_session(&state, addr).await;

    let park = format!("/v1/relay/{session_id}?role=accept");
    let (mut accepting, _) = tokio_tungstenite::connect_async(ws_request(addr, &park, &ticket))
        .await
        .unwrap();
    let dial = format!("/v1/relay/{session_id}?role=connect");
    let (mut connecting, _) = tokio_tungstenite::connect_async(ws_request(addr, &dial, &ticket))
        .await
        .unwrap();

    // Both learn they are paired.
    assert!(accepting
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("paired"));
    assert!(connecting
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("paired"));

    // Then go quiet. Within the idle bound the relay tears the pair down and
    // the peer sees the socket close rather than hanging.
    let closed = accepting
        .next()
        .await
        .map(|m| match m {
            Ok(msg) => msg.is_close(),
            Err(_) => true,
        })
        .unwrap_or(true);
    assert!(closed, "the relay should have dropped the idle connection");
}

#[tokio::test]
async fn an_old_client_is_told_to_update() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    // No x-filecommander-protocol header: the server reads it as version 0,
    // below the wire protocol it now speaks.
    let req = Request::builder()
        .method("POST")
        .uri("/v1/auth/login")
        .header("content-type", "application/json")
        .body(Body::from(
            json!({
                "email": "user@example.com",
                "password": "correct horse",
                "device_name": "old build",
                "platform": "linux",
                "device_id": "",
            })
            .to_string(),
        ))
        .unwrap();
    let response = router(state.clone()).oneshot(req).await.unwrap();
    assert_eq!(response.status(), StatusCode::UPGRADE_REQUIRED);
    assert_eq!(
        json_of(response).await["detail"],
        "Your version of FileCommander is too old. Please update to continue."
    );
}

#[tokio::test]
async fn a_peer_is_pushed_when_a_device_comes_online() {
    use futures_util::{SinkExt, StreamExt};

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "user@example.com", "correct horse").await;
    let host = login(&state, "user@example.com", "desktop", "").await;
    let guest = login(&state, "user@example.com", "laptop", "").await;

    // The guest finishes its hello first, so it is a registered peer by the
    // time the host comes online.
    let (mut guest_socket, _) = tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/agent",
        guest["access_token"].as_str().unwrap(),
    ))
    .await
    .unwrap();
    assert!(guest_socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("welcome"));
    guest_socket
        .send(tokio_tungstenite::tungstenite::Message::Text(
            json!({"type": "hello", "lan_addrs": [], "port": 45001})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
    assert!(guest_socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("ready"));

    // The host connects and announces itself; that announcement must be pushed
    // to the guest over the guest's own socket.
    let (mut host_socket, _) = tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/agent",
        host["access_token"].as_str().unwrap(),
    ))
    .await
    .unwrap();
    assert!(host_socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("welcome"));
    host_socket
        .send(tokio_tungstenite::tungstenite::Message::Text(
            json!({"type": "hello", "lan_addrs": [], "port": 45002})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
    assert!(host_socket
        .next()
        .await
        .unwrap()
        .unwrap()
        .to_text()
        .unwrap()
        .contains("ready"));

    let pushed: Value = serde_json::from_str(
        guest_socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(pushed["type"], "presence");
    assert_eq!(pushed["device_id"], host["device_id"].as_str().unwrap());
    assert_eq!(pushed["online"], true);
}

#[tokio::test]
async fn clipboard_is_account_scoped_and_serves_thumbnails_separately() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    register(&state, "other@example.com", "correct horse").await;
    let owner = login(&state, "owner@example.com", "desktop", "").await;
    let other = login(&state, "other@example.com", "laptop", "").await;
    let token = owner["access_token"].as_str().unwrap();
    let image = publish_image(&state, token, owner["device_id"].as_str().unwrap()).await;
    let id = image["id"].as_str().unwrap();

    let list = json_of(send(&state, "GET", "/v1/clipboard", token, Value::Null).await).await;
    assert_eq!(list["items"].as_array().unwrap().len(), 1);
    assert!(list["items"][0].get("thumbnail_base64").is_none());
    assert_eq!(
        list["items"][0]["thumbnail_url"],
        format!("/v1/clipboard/{id}/thumbnail")
    );

    let thumbnail = send(
        &state,
        "GET",
        &format!("/v1/clipboard/{id}/thumbnail"),
        token,
        Value::Null,
    )
    .await;
    assert_eq!(thumbnail.status(), StatusCode::OK);
    assert_eq!(thumbnail.headers()["content-type"], "image/png");
    assert_eq!(
        thumbnail
            .into_body()
            .collect()
            .await
            .unwrap()
            .to_bytes()
            .as_ref(),
        b"hello"
    );

    let other_token = other["access_token"].as_str().unwrap();
    assert!(
        json_of(send(&state, "GET", "/v1/clipboard", other_token, Value::Null).await).await
            ["items"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        send(
            &state,
            "GET",
            &format!("/v1/clipboard/{id}/thumbnail"),
            other_token,
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        send(
            &state,
            "DELETE",
            &format!("/v1/clipboard/{id}"),
            other_token,
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NOT_FOUND
    );

    let deleted = json_of(
        send(
            &state,
            "DELETE",
            &format!("/v1/clipboard/{id}"),
            token,
            Value::Null,
        )
        .await,
    )
    .await;
    let delta =
        json_of(send(&state, "GET", "/v1/clipboard?after=1", token, Value::Null).await).await;
    assert_eq!(delta["deleted_ids"], json!([id]));
    assert_eq!(delta["revision"], deleted["revision"]);

    assert_eq!(
        send(
            &state,
            "POST",
            "/v1/clipboard",
            token,
            json!({"type": "text", "text": "to clear"}),
        )
        .await
        .status(),
        StatusCode::OK
    );
    let before_clear = json_of(send(&state, "GET", "/v1/clipboard", token, Value::Null).await)
        .await["revision"]
        .as_i64()
        .unwrap();
    assert_eq!(
        send(&state, "DELETE", "/v1/clipboard", token, Value::Null)
            .await
            .status(),
        StatusCode::OK
    );
    assert!(json_of(
        send(
            &state,
            "GET",
            &format!("/v1/clipboard?after={before_clear}"),
            token,
            Value::Null,
        )
        .await,
    )
    .await["cleared"]
        .as_bool()
        .unwrap());
}

#[tokio::test]
async fn clipboard_enforces_limits_retention_and_expiry() {
    let state = state();
    register(&state, "user@example.com", "correct horse").await;
    let session = login(&state, "user@example.com", "desktop", "").await;
    let token = session["access_token"].as_str().unwrap();

    assert_eq!(
        send(
            &state,
            "POST",
            "/v1/clipboard",
            token,
            json!({"type": "text", "text": "x".repeat(64 * 1024 + 1)}),
        )
        .await
        .status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
    assert_eq!(
        send(
            &state,
            "POST",
            "/v1/clipboard",
            token,
            json!({"type": "text", "text": "x".repeat(300 * 1024)}),
        )
        .await
        .status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
    assert_eq!(
        send(
            &state,
            "POST",
            "/v1/clipboard",
            token,
            json!({
                "type": "image", "thumbnail_base64": "aGVsbG8=", "thumbnail_mime": "image/png",
                "mime": "image/jpeg", "size": 25 * 1024 * 1024 + 1, "width": 1, "height": 1,
                "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "source_device_id": session["device_id"],
            }),
        )
        .await
        .status(),
        StatusCode::BAD_REQUEST
    );

    for index in 0..21 {
        assert_eq!(
            send(
                &state,
                "POST",
                "/v1/clipboard",
                token,
                json!({"type": "text", "text": index.to_string()}),
            )
            .await
            .status(),
            StatusCode::OK
        );
    }
    let list = json_of(send(&state, "GET", "/v1/clipboard", token, Value::Null).await).await;
    assert_eq!(list["items"].as_array().unwrap().len(), 20);
    assert_ne!(list["items"][0]["text"], "0");
    let before_expiry = list["revision"].as_i64().unwrap();
    state
        .db
        .call(|conn| {
            conn.execute(
                "UPDATE clipboard_items SET expires = '2000-01-01T00:00:00+00:00'",
                [],
            )
            .unwrap();
        })
        .await;
    let expired = json_of(
        send(
            &state,
            "GET",
            &format!("/v1/clipboard?after={before_expiry}"),
            token,
            Value::Null,
        )
        .await,
    )
    .await;
    assert!(expired["items"].as_array().unwrap().is_empty());
    assert_eq!(expired["deleted_ids"].as_array().unwrap().len(), 20);
    assert!(expired["revision"].as_i64().unwrap() > before_expiry);
}

#[tokio::test]
async fn clipboard_image_sessions_are_source_bound_and_plain_sessions_unchanged() {
    use futures_util::{SinkExt, StreamExt};

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "user@example.com", "correct horse").await;
    let host = login(&state, "user@example.com", "desktop", "").await;
    let guest = login(&state, "user@example.com", "laptop", "").await;
    let other = login(&state, "user@example.com", "tablet", "").await;
    let (mut host_socket, _) = tokio_tungstenite::connect_async(ws_request(
        addr,
        "/v1/agent",
        host["access_token"].as_str().unwrap(),
    ))
    .await
    .unwrap();
    host_socket.next().await;
    host_socket
        .send(tokio_tungstenite::tungstenite::Message::Text(
            json!({"type": "hello", "lan_addrs": [], "port": 45001})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
    host_socket.next().await;

    let plain = json_of(
        send(
            &state,
            "POST",
            "/v1/session",
            guest["access_token"].as_str().unwrap(),
            json!({"device_id": host["device_id"]}),
        )
        .await,
    )
    .await;
    assert!(plain.get("clipboard_item_id").is_none());
    let plain_notice: Value = serde_json::from_str(
        host_socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap(),
    )
    .unwrap();
    assert!(plain_notice.get("clipboard_item_id").is_none());

    let image = publish_image(
        &state,
        host["access_token"].as_str().unwrap(),
        host["device_id"].as_str().unwrap(),
    )
    .await;
    let item_id = image["id"].as_str().unwrap();
    assert_eq!(
        send(
            &state,
            "POST",
            "/v1/session",
            guest["access_token"].as_str().unwrap(),
            json!({"device_id": other["device_id"], "clipboard_item_id": item_id}),
        )
        .await
        .status(),
        StatusCode::BAD_REQUEST
    );
    let image_session = json_of(
        send(
            &state,
            "POST",
            "/v1/session",
            guest["access_token"].as_str().unwrap(),
            json!({"device_id": host["device_id"], "clipboard_item_id": item_id}),
        )
        .await,
    )
    .await;
    assert_eq!(image_session["clipboard_item_id"], item_id);
    let notice: Value = serde_json::from_str(
        host_socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(notice["clipboard_item_id"], item_id);
}

#[tokio::test]
async fn clipboard_pushes_only_to_other_capable_devices() {
    use futures_util::StreamExt;

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "user@example.com", "correct horse").await;
    let source = login(&state, "user@example.com", "desktop", "").await;
    let capable = login(&state, "user@example.com", "laptop", "").await;
    let legacy = login(&state, "user@example.com", "tablet", "").await;

    async fn agent(
        addr: SocketAddr,
        token: &str,
        capabilities: Value,
    ) -> tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>
    {
        use futures_util::{SinkExt, StreamExt};
        let (mut socket, _) =
            tokio_tungstenite::connect_async(ws_request(addr, "/v1/agent", token))
                .await
                .unwrap();
        socket.next().await;
        socket
            .send(tokio_tungstenite::tungstenite::Message::Text(
                json!({"type": "hello", "lan_addrs": [], "port": 45001, "capabilities": capabilities})
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        socket.next().await;
        socket
    }

    let mut source_socket = agent(addr, source["access_token"].as_str().unwrap(), json!([])).await;
    let mut capable_socket = agent(
        addr,
        capable["access_token"].as_str().unwrap(),
        json!(["clipboard"]),
    )
    .await;
    // The capable device's hello generated this ordinary presence notification.
    source_socket.next().await;
    let mut legacy_socket = agent(addr, legacy["access_token"].as_str().unwrap(), json!([])).await;
    // Both existing sockets receive the legacy device's presence notification.
    source_socket.next().await;
    capable_socket.next().await;

    let published = send(
        &state,
        "POST",
        "/v1/clipboard",
        source["access_token"].as_str().unwrap(),
        json!({"type": "text", "text": "private text"}),
    )
    .await;
    assert_eq!(published.status(), StatusCode::OK);
    let pushed: Value = serde_json::from_str(
        capable_socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(pushed["type"], "clipboard_changed");
    assert_eq!(pushed["change"], "publish");
    assert!(pushed.get("text").is_none());
    assert!(
        tokio::time::timeout(Duration::from_millis(150), source_socket.next())
            .await
            .is_err()
    );
    assert!(
        tokio::time::timeout(Duration::from_millis(150), legacy_socket.next())
            .await
            .is_err()
    );
}

#[tokio::test]
async fn clipboard_delivery_send_queues_text_for_other_account_devices() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    register(&state, "other@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let laptop = login(&state, "owner@example.com", "laptop", "").await;
    let tablet = login(&state, "owner@example.com", "tablet", "").await;
    let other = login(&state, "other@example.com", "other", "").await;
    let source_token = source["access_token"].as_str().unwrap();
    let text = "hello from the desktop — 你好";

    let queued = send_text(&state, "/v1/clipboard/send", source_token, text).await;
    assert_eq!(queued.status(), StatusCode::OK);
    let queued = json_of(queued).await;
    assert_eq!(queued["recipient_count"], 2);
    let payload_id = queued["payload_id"]
        .as_str()
        .expect("payload id")
        .to_string();

    for recipient in [&laptop, &tablet] {
        let deliveries = json_of(
            send(
                &state,
                "GET",
                "/v1/clipboard/deliveries",
                recipient["access_token"].as_str().unwrap(),
                Value::Null,
            )
            .await,
        )
        .await;
        let rows = deliveries["deliveries"].as_array().expect("deliveries");
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["payload_id"], payload_id);
        assert_eq!(rows[0]["kind"], "text");
        assert_eq!(rows[0]["mime"], "text/plain; charset=utf-8");
        assert_eq!(rows[0]["size"], text.len());
        assert!(rows[0]["width"].is_null());
        assert!(rows[0]["height"].is_null());
        assert_eq!(rows[0]["source_device_id"], source["device_id"]);
        assert_eq!(rows[0]["source_device_name"], "desktop");
        assert!(rows[0]["sha256"].as_str().unwrap().len() == 64);

        let delivery_id = rows[0]["id"].as_str().unwrap();
        let content = send(
            &state,
            "GET",
            &format!("/v1/clipboard/deliveries/{delivery_id}/content"),
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await;
        assert_eq!(content.status(), StatusCode::OK);
        assert_eq!(
            content.headers()["content-type"],
            "text/plain; charset=utf-8"
        );
        assert_eq!(
            content
                .into_body()
                .collect()
                .await
                .unwrap()
                .to_bytes()
                .as_ref(),
            text.as_bytes()
        );
    }

    for session in [&source, &other] {
        let deliveries = json_of(
            send(
                &state,
                "GET",
                "/v1/clipboard/deliveries",
                session["access_token"].as_str().unwrap(),
                Value::Null,
            )
            .await,
        )
        .await;
        assert!(deliveries["deliveries"].as_array().unwrap().is_empty());
    }
}

#[tokio::test]
async fn clipboard_delivery_ack_is_device_owned_and_idempotent() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let recipient = login(&state, "owner@example.com", "laptop", "").await;
    let intruder = login(&state, "owner@example.com", "tablet", "").await;

    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "one pending delivery",
        )
        .await,
    )
    .await;
    assert_eq!(queued["recipient_count"], 2);
    let deliveries = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    let delivery_id = deliveries["deliveries"][0]["id"]
        .as_str()
        .unwrap()
        .to_string();
    let path = format!("/v1/clipboard/deliveries/{delivery_id}/ack");

    assert_eq!(
        send(
            &state,
            "POST",
            &path,
            intruder["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        send(
            &state,
            "POST",
            &path,
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NO_CONTENT
    );
    let pending = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    assert!(pending["deliveries"].as_array().unwrap().is_empty());
    assert_eq!(
        send(
            &state,
            "POST",
            &path,
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NO_CONTENT
    );
}

#[tokio::test]
async fn clipboard_delivery_send_without_recipients_keeps_no_payload() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;

    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "nobody else is logged in",
        )
        .await,
    )
    .await;
    assert_eq!(queued["recipient_count"], 0);
    let payload_id = queued["payload_id"]
        .as_str()
        .expect("payload id")
        .to_string();
    let retained = state
        .db
        .call(move |conn| {
            conn.query_row(
                "SELECT COUNT(*) FROM clipboard_payloads WHERE id = ?1",
                [payload_id],
                |row| row.get::<_, i64>(0),
            )
            .unwrap()
        })
        .await;
    assert_eq!(retained, 0);
}

#[tokio::test]
async fn clipboard_delivery_purges_expired_pending_payloads() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let recipient = login(&state, "owner@example.com", "laptop", "").await;
    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "expires without being delivered",
        )
        .await,
    )
    .await;
    let payload_id = queued["payload_id"].as_str().unwrap().to_string();
    state
        .db
        .call(move |conn| {
            conn.execute(
                "UPDATE clipboard_payloads SET expires = '2000-01-01T00:00:00+00:00' WHERE id = ?1",
                [payload_id],
            )
            .unwrap();
        })
        .await;

    let pending = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    assert!(pending["deliveries"].as_array().unwrap().is_empty());
    let (payloads, deliveries) = state
        .db
        .call(|conn| {
            (
                conn.query_row("SELECT COUNT(*) FROM clipboard_payloads", [], |row| {
                    row.get::<_, i64>(0)
                })
                .unwrap(),
                conn.query_row("SELECT COUNT(*) FROM clipboard_deliveries", [], |row| {
                    row.get::<_, i64>(0)
                })
                .unwrap(),
            )
        })
        .await;
    assert_eq!(payloads, 0);
    assert_eq!(deliveries, 0);
}

#[tokio::test]
async fn clipboard_delivery_removes_payload_after_every_recipient_acknowledges() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let recipient = login(&state, "owner@example.com", "laptop", "").await;
    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "remove after delivery",
        )
        .await,
    )
    .await;
    let payload_id = queued["payload_id"].as_str().unwrap().to_string();
    let deliveries = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    let delivery_id = deliveries["deliveries"][0]["id"].as_str().unwrap();
    let path = format!("/v1/clipboard/deliveries/{delivery_id}/ack");
    assert_eq!(
        send(
            &state,
            "POST",
            &path,
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await
        .status(),
        StatusCode::NO_CONTENT
    );
    let retained = state
        .db
        .call(move |conn| {
            conn.query_row(
                "SELECT COUNT(*) FROM clipboard_payloads WHERE id = ?1",
                [payload_id],
                |row| row.get::<_, i64>(0),
            )
            .unwrap()
        })
        .await;
    assert_eq!(retained, 0);
}

#[tokio::test]
async fn clipboard_delivery_accepts_exactly_64_kib_of_text() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let recipient = login(&state, "owner@example.com", "laptop", "").await;
    let text = vec![b'x'; 64 * 1024];

    let queued = send_raw(
        &state,
        "/v1/clipboard/send",
        source["access_token"].as_str().unwrap(),
        "text/plain; charset=utf-8",
        text,
    )
    .await;
    assert_eq!(queued.status(), StatusCode::OK);
    let deliveries = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    assert_eq!(deliveries["deliveries"][0]["size"], 64 * 1024);
}

#[tokio::test]
async fn clipboard_delivery_rejects_over_64_kib_after_purging_expired_payloads() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let _recipient = login(&state, "owner@example.com", "laptop", "").await;
    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "expired before an invalid send",
        )
        .await,
    )
    .await;
    let payload_id = queued["payload_id"].as_str().unwrap().to_string();
    state
        .db
        .call(move |conn| {
            conn.execute(
                "UPDATE clipboard_payloads SET expires = '2000-01-01T00:00:00+00:00' WHERE id = ?1",
                [payload_id],
            )
            .unwrap();
        })
        .await;

    let rejected = send_raw(
        &state,
        "/v1/clipboard/send",
        source["access_token"].as_str().unwrap(),
        "text/plain; charset=utf-8",
        vec![b'x'; 64 * 1024 + 1],
    )
    .await;
    assert_eq!(rejected.status(), StatusCode::PAYLOAD_TOO_LARGE);
    let retained = state
        .db
        .call(|conn| {
            conn.query_row("SELECT COUNT(*) FROM clipboard_payloads", [], |row| {
                row.get::<_, i64>(0)
            })
            .unwrap()
        })
        .await;
    assert_eq!(retained, 0);
}

#[tokio::test]
async fn clipboard_delivery_rejects_invalid_utf8() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;

    let rejected = send_raw(
        &state,
        "/v1/clipboard/send",
        source["access_token"].as_str().unwrap(),
        "text/plain; charset=utf-8",
        vec![0xff],
    )
    .await;
    assert_eq!(rejected.status(), StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn clipboard_delivery_rejects_wrong_content_type() {
    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;

    let rejected = send_raw(
        &state,
        "/v1/clipboard/send",
        source["access_token"].as_str().unwrap(),
        "application/json",
        b"not text/plain".to_vec(),
    )
    .await;
    assert_eq!(rejected.status(), StatusCode::UNSUPPORTED_MEDIA_TYPE);
}

#[tokio::test]
async fn clipboard_delivery_queues_an_image_for_only_its_account() {
    use sha2::{Digest, Sha256};

    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    register(&state, "other@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let recipient = login(&state, "owner@example.com", "laptop", "").await;
    let outsider = login(&state, "other@example.com", "other", "").await;
    let image = b"raw png bytes".to_vec();
    let digest = hex::encode(Sha256::digest(&image));

    let queued = send_image(
        &state,
        source["access_token"].as_str().unwrap(),
        "image/png",
        12,
        34,
        &digest,
        image.clone(),
    )
    .await;
    assert_eq!(queued.status(), StatusCode::OK);
    let queued = json_of(queued).await;
    assert_eq!(queued["recipient_count"], 1);

    let deliveries = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            recipient["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    let delivery = &deliveries["deliveries"][0];
    assert_eq!(delivery["payload_id"], queued["payload_id"]);
    assert_eq!(delivery["kind"], "image");
    assert_eq!(delivery["mime"], "image/png");
    assert_eq!(delivery["size"], image.len());
    assert_eq!(delivery["width"], 12);
    assert_eq!(delivery["height"], 34);
    assert_eq!(delivery["sha256"], digest);

    let delivery_id = delivery["id"].as_str().unwrap();
    let content = send(
        &state,
        "GET",
        &format!("/v1/clipboard/deliveries/{delivery_id}/content"),
        recipient["access_token"].as_str().unwrap(),
        Value::Null,
    )
    .await;
    assert_eq!(content.status(), StatusCode::OK);
    assert_eq!(content.headers()["content-type"], "image/png");
    assert_eq!(content.headers()["x-content-sha256"], digest);
    assert_eq!(
        content
            .into_body()
            .collect()
            .await
            .unwrap()
            .to_bytes()
            .as_ref(),
        image
    );

    let outsider_deliveries = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            outsider["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    assert!(outsider_deliveries["deliveries"]
        .as_array()
        .unwrap()
        .is_empty());
}

#[tokio::test]
async fn clipboard_delivery_rejects_invalid_image_metadata_and_large_bodies() {
    use sha2::{Digest, Sha256};

    let state = state();
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let token = source["access_token"].as_str().unwrap();
    let image = b"raw png bytes".to_vec();
    let digest = hex::encode(Sha256::digest(&image));

    assert_eq!(
        send_image(
            &state,
            token,
            "image/png",
            1,
            1,
            &"0".repeat(64),
            image.clone()
        )
        .await
        .status(),
        StatusCode::BAD_REQUEST
    );
    assert_eq!(
        send_image(&state, token, "image/png", 0, 1, &digest, image.clone())
            .await
            .status(),
        StatusCode::BAD_REQUEST
    );
    assert_eq!(
        send_image(
            &state,
            token,
            "image/png",
            1,
            1,
            &hex::encode(Sha256::digest(vec![b'x'; 25 * 1024 * 1024 + 1])),
            vec![b'x'; 25 * 1024 * 1024 + 1],
        )
        .await
        .status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
}

#[tokio::test]
async fn clipboard_delivery_notifies_each_online_target_once_but_not_the_sender() {
    use futures_util::{SinkExt, StreamExt};

    let state = state();
    let addr = serve(state.clone()).await;
    register(&state, "owner@example.com", "correct horse").await;
    let source = login(&state, "owner@example.com", "desktop", "").await;
    let target = login(&state, "owner@example.com", "laptop", "").await;

    async fn agent(
        addr: SocketAddr,
        token: &str,
    ) -> tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>
    {
        let (mut socket, _) =
            tokio_tungstenite::connect_async(ws_request(addr, "/v1/agent", token))
                .await
                .unwrap();
        socket.next().await;
        socket
            .send(tokio_tungstenite::tungstenite::Message::Text(
                json!({"type": "hello", "lan_addrs": [], "port": 45001})
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        socket.next().await;
        socket
    }

    let mut source_socket = agent(addr, source["access_token"].as_str().unwrap()).await;
    let mut target_socket = agent(addr, target["access_token"].as_str().unwrap()).await;
    source_socket.next().await;

    let queued = json_of(
        send_text(
            &state,
            "/v1/clipboard/send",
            source["access_token"].as_str().unwrap(),
            "notify the target",
        )
        .await,
    )
    .await;
    assert_eq!(queued["recipient_count"], 1);

    let notification: Value = serde_json::from_str(
        target_socket
            .next()
            .await
            .unwrap()
            .unwrap()
            .to_text()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(notification["type"], "clipboard_delivery");
    assert_eq!(notification["kind"], "text");
    assert_eq!(notification["size"], "notify the target".len());
    let target_delivery = json_of(
        send(
            &state,
            "GET",
            "/v1/clipboard/deliveries",
            target["access_token"].as_str().unwrap(),
            Value::Null,
        )
        .await,
    )
    .await;
    assert_eq!(
        notification["delivery_id"],
        target_delivery["deliveries"][0]["id"]
    );
    assert!(notification.get("content").is_none());
    assert!(
        tokio::time::timeout(Duration::from_millis(150), source_socket.next())
            .await
            .is_err()
    );
    assert!(
        tokio::time::timeout(Duration::from_millis(150), target_socket.next())
            .await
            .is_err()
    );
}
