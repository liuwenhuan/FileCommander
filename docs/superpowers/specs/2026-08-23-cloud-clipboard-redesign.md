# Cloud Clipboard Redesign

## Context

The current cloud clipboard treats the account server as a shared automatic history and uses a local text editor as an input surface. Image originals stay on the sending device and later move peer-to-peer through `FileShareServer`. This does not match the desired product and has produced confusing waits and a high-risk socket lifetime path.

The replacement separates three concepts:

1. **Local automatic history** — the most recent clipboard contents observed on this device.
2. **Targeted cross-device sends** — a user sends selected local history entries to one chosen account device or to all other account devices.
3. **Opt-in Auto Send** — when explicitly enabled, future accepted local clipboard changes use the currently selected target; existing history and signed-out captures are never replayed.
4. **Incoming deliveries** — content explicitly sent by another device, cached by the server for offline recipients and copied into the operating-system clipboard only after a user double-clicks it.

## Product Behavior

### Local history

- Observe the operating-system clipboard automatically.
- Persist the newest 50 records in a private local store across restarts.
- Admit only text and images.
- Prefer a real image over simultaneous HTML/text/URL fallback data.
- When no image exists, store only `QMimeData::text()`; rich HTML/RTF/document formats lose formatting.
- Reject files, file URLs, password/private MIME types, and empty/oversized values.
- Deduplicate by SHA-256. A duplicate updates its timestamp and moves to the front.
- Store image bytes in a private cache and metadata/text in a JSON manifest.
- Evict the oldest item and its image when the capacity exceeds 50.

### Panel

The existing popup remains anchored to the Quick Connect area but changes semantics:

- Search field.
- History list containing local records plus explicit incoming deliveries.
- Read-only preview area; no editable input box.
- Text preview uses a read-only text widget.
- Image preview scales the `QImage` before creating a pixmap.
- Single-click selects and previews an item.
- Double-click copies that item into the local operating-system clipboard.
- A selected local item exposes a target selector, **Send**, and an opt-in **Auto Send** control.
- The selector lists all non-self account devices, including offline devices, plus an **All devices** choice; online/offline state is visible.
- An incoming item cannot be manually or automatically rebroadcast.
- The panel shows encoding/upload/download/delivery state and a progress bar.

### Sending

- Manual Send targets either one selected account device or **All devices**; the sender is always excluded.
- The target list includes online and offline devices and displays their state. Offline recipients receive durable pending deliveries from the server.
- Text is uploaded as UTF-8.
- Images upload their normalized encoded bytes plus MIME, dimensions, size and SHA-256.
- Existing limits remain: 25 MiB encoded image, max dimension 16384, max 40 million pixels, text max 64 KiB.
- The UI reports encoding, upload progress, and final server-queued state.
- Auto Send is a new, default-off opt-in setting. Once enabled, only future accepted local clipboard changes use the target selected at capture time.
- Enabling Auto Send does not send existing history/current preview; signed-out captures are not replayed; incoming deliveries and controller-originated clipboard copies are never rebroadcast.
- Disabling Auto Send removes not-yet-started automatic work but does not cancel an upload already handed to the network stack or remove manual sends.

### Incoming deliveries

- Online devices receive a WebSocket notification and fetch pending deliveries.
- Offline devices query pending deliveries after login/agent ready.
- Text is downloaded and added to local history immediately.
- Images stream to a private temporary file with byte progress, validate size and SHA-256, then atomically enter the image cache/history.
- Successful local storage is acknowledged to the server.
- Incoming data never automatically overwrites the operating-system clipboard.
- The user double-clicks an incoming record to put it into the clipboard.

## Server Model

### Payload

`clipboard_payloads`

- `id`
- `user_id`
- `source_device_id`
- `kind` (`text` or `image`)
- text or image blob
- MIME, size, width, height, SHA-256
- optional thumbnail
- created and expires timestamps

### Per-device delivery

`clipboard_deliveries`

- `payload_id`
- `target_device_id`
- state (`pending` or `delivered`)
- delivered timestamp

A send creates one payload and one pending delivery for the chosen device, or one delivery per other account device for **All devices**. Content is account-scoped. Payloads expire after seven days and are removed once all deliveries are delivered or expired.

## API Contract

- `POST /v1/clipboard/send` — create payload and deliveries for all other registered account devices.
- `POST /v1/clipboard/send-targeted?target={device_id}` — fail-closed delivery to exactly one account-owned non-self device; an old server cannot silently broadcast this request.
- `GET /v1/clipboard/deliveries` — pending delivery metadata for the authenticated device.
- `GET /v1/clipboard/deliveries/{id}/content` — stream text/image content with content length, MIME and digest headers.
- `POST /v1/clipboard/deliveries/{id}/ack` — mark delivered after verified local storage.
- WebSocket frame `clipboard_delivery` contains delivery id and lightweight metadata, never original bytes.

Legacy `/v1/clipboard` endpoints remain temporarily for compatibility but the redesigned UI does not use them. The old point-to-point clipboard ticket/session flow is retired after the new server delivery path is covered.

## Client Components

### `ClipboardHistoryStore` (core)

- Owns manifest and image files.
- Performs admission, hash deduplication, capacity eviction, atomic manifest writes, and recovery from missing/corrupt image files.
- Exposes immutable records and add/remove/lookup operations.

### `CloudClipboardController` (UI policy)

- Observes `QClipboard::dataChanged`.
- Converts MIME data to local history records through the store.
- Coordinates manual/automatic targeted sends and pending receives through `AccountClient`.
- Persists the default-off Auto Send setting and stable target device ID; queued sends snapshot their target and origin.
- Guards against feedback loops when it writes a selected history record back to `QClipboard`.

### `AccountClient` (transport)

- Adds delivery API methods and upload/download progress signals.
- Streams image payloads rather than building Base64 JSON.
- Queries pending deliveries on login and agent ready.
- ACKs only after local verification succeeds.

### `NotepadPanel` (view)

- Becomes a history/preview UI only.
- Contains no editable composition state; the only automatic control is the new default-off Auto Send checkbox for future local captures.
- Maps list actions to preview, copy, target selection, manual/automatic send, delete-local-history, and progress display.

## Failure Handling

- Local clipboard admission failures are ignored without disrupting the existing clipboard.
- Store writes are atomic; a corrupt manifest is quarantined and rebuilt from valid records.
- Upload failures keep the local record and show retryable status.
- Download failures keep delivery pending; reconnect or manual retry can fetch again.
- Hash/size mismatch deletes the partial file and never ACKs.
- Server delivery creation is transactional: payload and recipient rows either all exist or none exist.
- No target devices means the send completes locally with an explicit “No other devices” state and no server payload.

## Migration

- Stop consuming global automatic server history in the new UI.
- Retired auto-upload/auto-receive keys never enable the new Auto Send option; the new setting starts disabled and requires an explicit checkbox action.
- Existing local cloud-image cache can be migrated or cleaned separately; the redesigned store uses a versioned directory/schema.
- Keep legacy server routes during one compatibility window; targeted clients use a distinct route so an old server fails rather than broadcasting.

## Testing

### Core

- MIME admission for image-first and rich-text-to-plain-text rules.
- Persistent 50-item history, deduplication, restart round-trip, eviction, private permissions, corrupt manifest recovery.
- Image cache atomicity, hash verification and orphan cleanup.

### Server

- All-device send excludes the sender; targeted send creates a delivery only for the selected account-owned device and rejects self/unknown/foreign targets.
- Account isolation and device ownership, including selected online-only notification and offline selected-device retrieval.
- Offline pending query, streamed content, ACK, duplicate ACK, TTL cleanup.
- Size/hash/type rejection and transactional rollback.

### UI/transport

- Single-click preview, double-click clipboard copy, multi-select deletion, local-only manual send, and target dropdown state.
- Default-off future-only Auto Send for text/images, target snapshotting, disable/logout races, and no incoming/controller-copy rebroadcast.
- Text/image upload progress and incoming download progress.
- Login/agent-ready pending fetch.
- Feedback-loop suppression when writing to local clipboard.
- No editable input or auto-sync controls remain.

## Work Decomposition

1. **Server delivery squad** — schema, API, WebSocket notification, retention and server tests.
2. **Local history squad** — store, MIME admission, persistence/cache and core tests.
3. **Transport squad** — `AccountClient` upload/download/progress/pending/ACK APIs and transport tests.
4. **Panel/controller squad** — redesigned controller/view interactions and UI tests.
5. **Integration pass** — align contracts, remove old flow usage, run targeted suites, security/data-loss review and package a Windows test binary.
