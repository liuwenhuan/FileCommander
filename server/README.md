# FileCommander account server

Accounts, the device registry, peer file-transfer signalling, and Cloud Clipboard
storage/delivery for FileCommander. One Rust binary with an embedded SQLite
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
| `FILECOMMANDER_UPDATE_ROOT` | — | read-only root containing `version.json`, `update.html`, and release packages |
| `FILECOMMANDER_PUBLIC_HOST` | `fc.aigutta.com` | fixed host used by the optional HTTP-to-HTTPS redirect listener |
| `FILECOMMANDER_PUBLIC_HTTP_BIND` | — | optional redirect-only HTTP bind address, e.g. `0.0.0.0:80` |
| `FILECOMMANDER_REQUIRE_TLS` | — | `true` rejects a startup without both TLS files |

With both TLS variables set the server serves HTTPS through rustls. With
either missing it serves plain HTTP, which sends access tokens across the wire
in the clear — acceptable for a local test run, never for a deployment. Set
`FILECOMMANDER_REQUIRE_TLS=true` in production. If `FILECOMMANDER_PUBLIC_HTTP_BIND`
is configured, it serves only permanent redirects to the configured HTTPS host.

## Public update files

When `FILECOMMANDER_UPDATE_ROOT` is configured, the server exposes only these
unauthenticated read-only paths in addition to its `/v1` API:

- `GET /version.json` — no-cache update announcement manifest;
- `GET /SHA256SUMS.txt` — no-cache checksum text;
- `GET /update.html` — static update/download page;
- `GET /updata.html` — permanent redirect to the correctly spelled page;
- `GET /releases/<version>/<filename>` — legacy immutable package alias with byte-range support;
- `GET /<package-filename>` — canonical root-level immutable package/checksum download with byte-range support.

The service user must be able to read but never write the update root. Publish
packages first and atomically replace `version.json` last, so a manifest never
points at a partial release. Directory listings, paths outside the configured
root, and symlinks are rejected.

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
| `POST /v1/clipboard/send` | queues one explicit Clipboard payload for every other registered device on the account; see below |
| `POST /v1/clipboard/send-targeted?target=<device_id>` | queues one explicit Clipboard payload for exactly one account-owned non-self device; see below |
| `GET /v1/clipboard/deliveries` | this device's pending Clipboard deliveries |
| `GET /v1/clipboard/deliveries/{delivery_id}/content` | the pending delivery's original bytes |
| `POST /v1/clipboard/deliveries/{delivery_id}/ack` | acknowledges this device's delivery (`204`) |

Access tokens last 15 minutes, refresh tokens 90 days, and refreshing rotates
both — a replayed refresh token is rejected. `login` re-claims a `device_id`
the account already owns and otherwise issues a new one, so signing in twice
on one machine does not create a second device row.

### Account email format

Registration and login accept syntax-only, ASCII email addresses. The server
trims surrounding whitespace and stores the address in lowercase. An address
must contain exactly one `@`; its local part is 1–64 characters and its domain
has at least two DNS-style labels. Dots cannot begin, end, or repeat in the
local part; domain labels are 1–63 ASCII alphanumeric/hyphen characters that
begin and end with an alphanumeric character. Unicode/EAI addresses, IP
literals, display names, comments, and whitespace inside the address are not
accepted. Already-ASCII punycode and reserved syntactic domains such as
`example.invalid` are accepted. This is format validation only: the server
does not query MX, DNS, public suffix, or mailbox existence.

### Presence: `WS /v1/agent`

Authenticated by the `Authorization` header. The connection is checked before
the upgrade, so a bad token fails as an ordinary HTTP error; credentials are not
accepted in the WebSocket URL query, where they could be logged.

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

### Cloud Clipboard explicit delivery

This is separate from the peer file-transfer flow above: it does not create a
`/v1/session` or use the relay. The normal Clipboard history flow also remains
separate. `POST /v1/clipboard/send` uploads a payload to the account server so
every other registered device can receive it, including devices that are offline
when it is sent. `POST /v1/clipboard/send-targeted?target=<device_id>` instead
queues the same payload for exactly one non-self device owned by the account;
it also accepts offline targets. Targeted sends never fall back to broadcast, so
a client using that route against an older server fails safely rather than
sending to unintended devices.

Both delivery routes take the original bytes, not JSON. Use
`Content-Type: text/plain; charset=utf-8` for UTF-8 text, or a valid
`image/*` media type for an image. Image requests also require
`X-Clipboard-Width`, `X-Clipboard-Height`, and the lowercase-or-uppercase
64-hex-character `X-Clipboard-SHA256` of the body. The response is
`{payload_id, recipient_count}`; the sender is excluded, and a zero recipient
count retains no payload.

- Text is limited to 64 KiB. Images are limited to 25 MiB, 16,384 pixels per
  dimension, and 40,000,000 pixels total. Invalid UTF-8, media types,
  dimensions, hashes, or empty/invalid UTF-8 text are rejected.
- `GET /v1/clipboard/deliveries` lists only the authenticated device's pending
  deliveries. Each entry supplies its `id`, `payload_id`, type, MIME type,
  byte size, optional dimensions, SHA-256, source device ID/name, and creation
  and expiry timestamps.
- `GET /v1/clipboard/deliveries/{delivery_id}/content` returns the original
  pending bytes with their `Content-Type`, `Content-Length`, and
  `X-Content-SHA256`. A delivery cannot be listed, downloaded, or acknowledged
  by another device, even on the same account.
- After the recipient has safely applied the payload, it calls
  `POST /v1/clipboard/deliveries/{delivery_id}/ack`, which returns `204`. ACK
  is idempotent for the seven-day delivery retention window. The delivery then
  leaves that device's pending list, and the stored payload is deleted as soon
  as every recipient has acknowledged it. Delivered metadata is also purged
  after that window.

An online recipient with an authenticated `WS /v1/agent` connection receives
one lightweight notification per delivery:

```json
{"type":"clipboard_delivery","delivery_id":"…","kind":"text","size":123}
```

The notification contains no content; the recipient fetches it through the
pending-delivery API. Offline recipients receive no WebSocket notification, but
their pending delivery remains available when they reconnect and poll the list.
Payloads and their pending deliveries expire after seven days; expiry deletes
them even if a recipient never acknowledged them.

### Cloud Clipboard privacy

Unlike peer file transfer (direct TLS or relayed ciphertext), explicit
Clipboard delivery makes the account server retain the original text or image
bytes in its SQLite database until all recipients acknowledge them or the
seven-day expiry is purged. It is therefore not end-to-end encrypted from the
server operator. TLS protects requests in transit when the server is deployed
with its configured certificate, and bearer authentication/account-scoped
recipient checks restrict access, but users should not send content they do not
want stored on that server for this retention window.

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
