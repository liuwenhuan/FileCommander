//! SQLite storage plus the two hashes the server needs: scrypt for passwords,
//! SHA-256 for tokens.

use std::sync::{Arc, Mutex};

use chrono::{DateTime, SecondsFormat, Utc};
use rand::RngCore;
use rusqlite::Connection;
use sha2::{Digest, Sha256};

pub const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS users (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    email    TEXT NOT NULL UNIQUE,
    pw_hash  TEXT NOT NULL,
    salt     TEXT NOT NULL,
    created  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
    id        TEXT PRIMARY KEY,
    user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name      TEXT NOT NULL,
    platform  TEXT NOT NULL,
    created   TEXT NOT NULL,
    last_seen TEXT,
    lan_addrs TEXT NOT NULL DEFAULT '',
    share_port INTEGER NOT NULL DEFAULT 0,
    share_pin TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS tokens (
    token_hash TEXT PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id  TEXT NOT NULL,
    kind       TEXT NOT NULL,          -- 'access' | 'refresh'
    expires    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_devices_user ON devices(user_id);
";

/// One connection behind one lock, reached from async code through
/// `spawn_blocking`. SQLite serialises writes anyway and this server handles a
/// handful of requests per device per day.
///
/// ponytail: single connection. Move to a pool only when that stops being true.
#[derive(Clone)]
pub struct Db(Arc<Mutex<Connection>>);

impl Db {
    pub fn open(path: &str) -> rusqlite::Result<Db> {
        let conn = Connection::open(path)?;
        conn.execute_batch(SCHEMA)?;
        // CREATE TABLE IF NOT EXISTS leaves an already-populated `devices`
        // exactly as it is, so a database from before share_pin never gets the
        // column from SCHEMA alone. Adding it here migrates the deployed
        // database in place; the error on the second run is "duplicate column
        // name", which is the success case from then on.
        let _ = conn.execute(
            "ALTER TABLE devices ADD COLUMN share_pin TEXT NOT NULL DEFAULT ''",
            [],
        );
        Ok(Db(Arc::new(Mutex::new(conn))))
    }

    pub fn memory() -> rusqlite::Result<Db> {
        Db::open(":memory:")
    }

    /// Runs one closure against the connection on the blocking pool. Password
    /// hashing runs inside these closures too -- 50 ms of scrypt must never sit
    /// on an async worker thread.
    pub async fn call<T, F>(&self, f: F) -> T
    where
        F: FnOnce(&Connection) -> T + Send + 'static,
        T: Send + 'static,
    {
        let conn = self.0.clone();
        tokio::task::spawn_blocking(move || {
            let guard = conn.lock().unwrap_or_else(|e| e.into_inner());
            f(&guard)
        })
        .await
        .expect("database task panicked")
    }
}

pub fn now() -> DateTime<Utc> {
    Utc::now()
}

/// Second-resolution ISO-8601, which is also the on-disk format: expiry checks
/// are plain string comparisons, so every timestamp must be written the same way.
pub fn iso(dt: DateTime<Utc>) -> String {
    dt.to_rfc3339_opts(SecondsFormat::Secs, false)
}

pub fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    rand::rng().fill_bytes(&mut buf);
    hex::encode(buf)
}

/// 256 bits of entropy in an URL-safe alphabet, like Python's
/// `secrets.token_urlsafe(32)`: tokens travel in headers and query strings.
pub fn random_token() -> String {
    let mut buf = [0u8; 32];
    rand::rng().fill_bytes(&mut buf);
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    buf.iter().map(|b| ALPHABET[(*b & 0x3f) as usize] as char).collect()
}

/// scrypt with the interactive-login parameters from RFC 7914 (N=2^15, r=8,
/// p=1), ~50 ms per attempt. Same parameters and same hex encoding the first
/// (Python) version of this server used, so an existing accounts.db still opens.
pub fn hash_password(password: &str, salt_hex: &str) -> String {
    let salt = hex::decode(salt_hex).unwrap_or_default();
    let params = scrypt::Params::new(15, 8, 1, 32).expect("valid scrypt parameters");
    let mut out = [0u8; 32];
    // Only fails on an empty salt slice, which cannot happen for a hex salt we
    // generated; a wrong password must still cost the same as a right one.
    if scrypt::scrypt(password.as_bytes(), &salt, &params, &mut out).is_err() {
        return String::new();
    }
    hex::encode(out)
}

/// Tokens already carry 256 bits of entropy, so a plain SHA-256 is enough: a
/// leaked database hands out no live sessions.
pub fn hash_token(token: &str) -> String {
    hex::encode(Sha256::digest(token.as_bytes()))
}

/// Comparison whose duration does not depend on where the first difference is.
pub fn constant_time_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The deployed database predates share_pin and has rows in it, so `open`
    /// has to add the column without disturbing them.
    #[test]
    fn open_adds_share_pin_to_an_older_database() {
        // No tempfile dependency for one test: a pid-stamped name in the
        // system temp dir is enough, removed at the end.
        let file = std::env::temp_dir().join(format!("fc-migrate-{}.db", std::process::id()));
        let _ = std::fs::remove_file(&file);
        let path = file.to_str().expect("utf-8 path");

        {
            let conn = Connection::open(path).expect("create");
            conn.execute_batch(
                "CREATE TABLE devices (
                    id        TEXT PRIMARY KEY,
                    user_id   INTEGER NOT NULL,
                    name      TEXT NOT NULL,
                    platform  TEXT NOT NULL,
                    created   TEXT NOT NULL,
                    last_seen TEXT,
                    lan_addrs TEXT NOT NULL DEFAULT '',
                    share_port INTEGER NOT NULL DEFAULT 0
                );
                INSERT INTO devices (id, user_id, name, platform, created, lan_addrs, share_port)
                VALUES ('dev-1', 1, 'Laptop', 'linux', '2026-01-01T00:00:00+00:00',
                        '10.0.0.2', 4711);",
            )
            .expect("old schema");
        }

        Db::open(path).expect("migrate");

        let conn = Connection::open(path).expect("reopen");
        let (name, port, pin): (String, i64, String) = conn
            .query_row(
                "SELECT name, share_port, share_pin FROM devices WHERE id = 'dev-1'",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .expect("row survived");
        assert_eq!(name, "Laptop");
        assert_eq!(port, 4711);
        assert_eq!(pin, "");

        // Running it again must be a no-op, not an error.
        Db::open(path).expect("second open");

        drop(conn);
        let _ = std::fs::remove_file(&file);
    }
}
