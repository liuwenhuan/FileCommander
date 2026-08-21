# FileCommander account server

Accounts, the device registry, and transfer signalling for FileCommander's
device-to-device transfer feature. One Rust binary with an embedded SQLite
database; nothing else has to be installed on the host.

It deliberately does not depend on a reverse proxy: TLS termination and
sign-in rate limiting are built in, so `nginx` is optional rather than
assumed.

## Build and run

```bash
cargo build --release
FILECOMMANDER_ACCOUNT_DB=/var/lib/filecommander/accounts.db \
FILECOMMANDER_ACCOUNT_BIND=0.0.0.0:8443 \
FILECOMMANDER_ACCOUNT_TLS_CERT=/etc/filecommander/fullchain.pem \
FILECOMMANDER_ACCOUNT_TLS_KEY=/etc/filecommander/privkey.pem \
./target/release/filecommander-account
```

| Variable | Default | Meaning |
|---|---|---|
| `FILECOMMANDER_ACCOUNT_BIND` | `0.0.0.0:8443` | listen address |
| `FILECOMMANDER_ACCOUNT_DB` | `accounts.db` | SQLite file, created on first run |
| `FILECOMMANDER_ACCOUNT_TLS_CERT` | — | PEM certificate chain |
| `FILECOMMANDER_ACCOUNT_TLS_KEY` | — | PEM private key |

With both TLS variables set the server serves HTTPS through rustls. With
either missing it serves plain HTTP, which sends access tokens across the wire
in the clear — acceptable for a local test run, never for a deployment.

Point a client at it with `FILECOMMANDER_ACCOUNT_API_URL`, or compile one with
`-DFILECOMMANDER_ACCOUNT_API_URL=https://…`.

## API

Errors are always `{"detail": "…"}` with a message fit to show a user; the Qt
client displays that field verbatim. Authenticated calls send
`Authorization: Bearer <access_token>`.

| Endpoint | Result |
|---|---|
| `POST /v1/auth/register` | `201 {"email": …}`; `409` if the address is taken |
| `POST /v1/auth/login` | `{access_token, refresh_token, device_id, expires_in}` |
| `POST /v1/auth/refresh` | the same, rotated; the old refresh token is spent |
| `POST /v1/auth/logout` | `204`, revoking this device's tokens |
| `GET /v1/devices` | bare JSON array of `{id, name, platform, online, self, last_seen, lan_addrs}` |
| `DELETE /v1/devices/{id}` | `204`, or `404` if the account does not own it |
| `WS /v1/agent` | presence socket, see below |
| `POST /v1/session` | `{session_id, ticket, expires_in, peer_lan_addrs, peer_port, peer_pin}` |
| `WS /v1/relay/{session_id}` | byte pipe between two devices with no direct route |

Access tokens last 15 minutes, refresh tokens 90 days, and refreshing rotates
both — a replayed refresh token is rejected. `login` re-claims a `device_id`
the account already owns and otherwise issues a new one, so signing in twice
on one machine does not create a second device row.

### Presence: `WS /v1/agent`

Authenticated by the `Authorization` header, or by `?token=` for clients that
cannot set headers on a WebSocket handshake. The connection is checked before
the upgrade, so a bad token fails as an ordinary HTTP error.

The server sends `{"type":"welcome"}` on connect. The client then sends

```json
{"type": "hello", "lan_addrs": ["192.168.1.7"], "port": 45001, "pin": "sha256//…"}
```

naming the addresses and port its own file-share server listens on, plus the
pin of the self-signed certificate that share serves (curl's `--pinnedpubkey`
form). The pin is stored and handed to a peer as `peer_pin`, so the peer can
pin the certificate without any CA in the picture. It gets
`{"type":"ready"}` back. A `{"type":"ping"}` every so often keeps the device
marked online (`{"type":"pong"}` in reply); a device counts as online for 90
seconds after its last message. Reconnecting replaces the previous socket, and
a stale socket closing afterwards cannot evict the newer one.

### Opening a transfer: `POST /v1/session`

`{"device_id": "…"}` names another device on the same account. The server
mints a ticket valid for 5 minutes, pushes it to that device over its agent
socket as

```json
{"type": "incoming", "session_id": …, "ticket": …, "from": …, "from_device": …, "expires_in": 300}
```

and returns the same ticket to the caller along with the target's LAN
addresses, port and pin. The caller then talks WebDAV over TLS to the target
directly, pinning that certificate and using the ticket as its bearer token; the target accepts it because it was handed
the ticket over a channel only the server can write to. There is no
ticket-verification endpoint, and the server never sees the transfer itself.

Devices with no direct route fall back to `WS /v1/relay/{session_id}`, which
splices two sockets bearing the same ticket into one byte pipe. The transfer
is TLS end-to-end, so the relay carries ciphertext it cannot read.

## Security notes

- Passwords are hashed with scrypt (N=2^15, r=8, p=1) and a per-user salt.
  That costs roughly 50 ms per attempt, so both hashing and every database
  call run on the blocking pool rather than an async worker.
- An unknown email is hashed against a dummy salt and answered with the same
  `401` as a wrong password, so sign-in does not reveal which addresses exist.
- `/v1/auth/register`, `/login` and `/refresh` are limited to 20 attempts per
  minute per source address.
- Tokens are stored as SHA-256 hashes and compared in constant time; the
  database never holds a usable token.

## Tests

```bash
cargo test
```

`tests/api.rs` drives the router in-process for the REST endpoints and binds a
real loopback port for the agent/session pair.
