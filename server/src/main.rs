//! Entry point. Terminates its own TLS and serves everything itself; there is
//! no reverse proxy in front of this process.

use std::net::SocketAddr;
use std::time::Duration;

use axum::{extract::OriginalUri, response::Redirect, Router};
use filecommander_account::{
    clipboard::purge_expired_delivery_payloads,
    db::{iso, now, Db},
    router,
    updates::UpdateRoot,
    AppState,
};

fn env_or(name: &str, fallback: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| fallback.to_string())
}

fn enabled(name: &str) -> bool {
    matches!(
        std::env::var(name).as_deref(),
        Ok("1") | Ok("true") | Ok("yes")
    )
}

async fn redirect_to_https(OriginalUri(uri): OriginalUri, host: String) -> Redirect {
    let path = uri
        .path_and_query()
        .map(|value| value.as_str())
        .unwrap_or("/");
    Redirect::permanent(&format!("https://{host}{path}"))
}

#[tokio::main]
async fn main() {
    let addr: SocketAddr = env_or("FILECOMMANDER_ACCOUNT_BIND", "0.0.0.0:8443")
        .parse()
        .expect("FILECOMMANDER_ACCOUNT_BIND must be host:port");
    let db_path = env_or("FILECOMMANDER_ACCOUNT_DB", "accounts.db");
    let cert = std::env::var("FILECOMMANDER_ACCOUNT_TLS_CERT").ok();
    let key = std::env::var("FILECOMMANDER_ACCOUNT_TLS_KEY").ok();
    let update_root = std::env::var("FILECOMMANDER_UPDATE_ROOT")
        .ok()
        .map(UpdateRoot::open)
        .transpose()
        .expect("FILECOMMANDER_UPDATE_ROOT must name a readable directory");
    let public_http_bind = std::env::var("FILECOMMANDER_PUBLIC_HTTP_BIND").ok();
    let public_host = env_or("FILECOMMANDER_PUBLIC_HOST", "fc.aigutta.com");
    if enabled("FILECOMMANDER_REQUIRE_TLS") && (cert.is_none() || key.is_none()) {
        panic!("FILECOMMANDER_REQUIRE_TLS requires both TLS certificate and key");
    }
    if public_http_bind.is_some() && (cert.is_none() || key.is_none()) {
        panic!("FILECOMMANDER_PUBLIC_HTTP_BIND requires TLS");
    }

    let db = Db::open(&db_path).expect("could not open the account database");
    let startup_cleanup = iso(now());
    db.call(move |conn| purge_expired_delivery_payloads(conn, &startup_cleanup))
        .await
        .expect("could not purge expired clipboard deliveries");
    let cleanup_db = db.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(60 * 60)).await;
            let stamp = iso(now());
            if let Err(error) = cleanup_db
                .call(move |conn| purge_expired_delivery_payloads(conn, &stamp))
                .await
            {
                eprintln!("could not purge expired clipboard deliveries: {error}");
            }
        }
    });
    let app_state = if let Some(root) = update_root {
        AppState::new(db).with_update_root(root)
    } else {
        AppState::new(db)
    };
    let app = router(app_state).into_make_service_with_connect_info::<SocketAddr>();

    if let Some(http_bind) = public_http_bind {
        let http_addr: SocketAddr = http_bind
            .parse()
            .expect("FILECOMMANDER_PUBLIC_HTTP_BIND must be host:port");
        let redirect = Router::new()
            .fallback(move |uri: OriginalUri| redirect_to_https(uri, public_host.clone()));
        tokio::spawn(async move {
            axum_server::bind(http_addr)
                .serve(redirect.into_make_service())
                .await
                .expect("HTTP redirect listener failed");
        });
    }

    match (cert, key) {
        (Some(cert), Some(key)) => {
            rustls::crypto::ring::default_provider()
                .install_default()
                .expect("could not install the rustls crypto provider");
            let config = axum_server::tls_rustls::RustlsConfig::from_pem_file(&cert, &key)
                .await
                .expect("could not read the TLS certificate or key");
            println!("filecommander-account: https://{addr} (db {db_path})");
            axum_server::bind_rustls(addr, config)
                .serve(app)
                .await
                .expect("server failed");
        }
        _ => {
            // Plain HTTP is for a local run or for a deployment that really has
            // something else terminating TLS. Tokens travel in the clear here.
            println!("filecommander-account: http://{addr} (db {db_path}) -- no TLS configured");
            axum_server::bind(addr)
                .serve(app)
                .await
                .expect("server failed");
        }
    }
}
