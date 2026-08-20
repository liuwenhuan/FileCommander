//! Entry point. Terminates its own TLS and serves everything itself; there is
//! no reverse proxy in front of this process.

use std::net::SocketAddr;

use filecommander_account::{db::Db, router, AppState};

fn env_or(name: &str, fallback: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| fallback.to_string())
}

#[tokio::main]
async fn main() {
    let addr: SocketAddr = env_or("FILECOMMANDER_ACCOUNT_BIND", "0.0.0.0:8443")
        .parse()
        .expect("FILECOMMANDER_ACCOUNT_BIND must be host:port");
    let db_path = env_or("FILECOMMANDER_ACCOUNT_DB", "accounts.db");
    let cert = std::env::var("FILECOMMANDER_ACCOUNT_TLS_CERT").ok();
    let key = std::env::var("FILECOMMANDER_ACCOUNT_TLS_KEY").ok();

    let db = Db::open(&db_path).expect("could not open the account database");
    // ConnectInfo is what the rate limiter keys on; without this the whole
    // server shares one bucket.
    let app = router(AppState::new(db))
        .into_make_service_with_connect_info::<SocketAddr>();

    match (cert, key) {
        (Some(cert), Some(key)) => {
            rustls::crypto::ring::default_provider()
                .install_default()
                .expect("could not install the rustls crypto provider");
            let config = axum_server::tls_rustls::RustlsConfig::from_pem_file(&cert, &key)
                .await
                .expect("could not read the TLS certificate or key");
            println!("filecommander-account: https://{addr} (db {db_path})");
            axum_server::bind_rustls(addr, config).serve(app).await.expect("server failed");
        }
        _ => {
            // Plain HTTP is for a local run or for a deployment that really has
            // something else terminating TLS. Tokens travel in the clear here.
            println!("filecommander-account: http://{addr} (db {db_path}) -- no TLS configured");
            axum_server::bind(addr).serve(app).await.expect("server failed");
        }
    }
}
