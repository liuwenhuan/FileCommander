# Cloud Clipboard Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace automatic account-wide clipboard mirroring with a persistent local 50-item history and explicit all-device delivery with offline server caching and transfer progress.

**Architecture:** A new core `ClipboardHistoryStore` owns local records and image files. `CloudClipboardController` captures the OS clipboard and coordinates explicit sends/incoming deliveries through new `AccountClient` transport methods. The Rust server stores one payload and one per-target delivery, notifying online devices and serving pending content to offline devices later. `NotepadPanel` becomes a read-only history/preview surface.

**Tech Stack:** Qt 5/C++17, QClipboard/QMimeData/QSaveFile/QNetworkAccessManager/QtConcurrent, Rust 2021, Axum 0.8, Tokio, rusqlite, GoogleTest, Rust integration tests.

**Spec:** `docs/superpowers/specs/2026-08-23-cloud-clipboard-redesign.md`

## Global Constraints

- Local history capacity is exactly 50 records and persists across restarts in a private config/cache directory.
- Admit only real images or plain text; image wins over simultaneous text/HTML fallback; files, file URLs and private/password MIME formats are rejected.
- Explicit sends target every other device in the account and exclude the sender.
- Incoming delivery never overwrites the OS clipboard automatically; double-click is the only copy-to-clipboard action.
- Server payload TTL is seven days; text max 64 KiB; image max 25 MiB, max dimension 16384 and max 40 million pixels.
- Image upload/download must stream with progress and validate SHA-256 before ACK.
- Legacy `/v1/clipboard` routes remain temporarily; new UI/controller must not use them.
- No new third-party dependencies unless explicitly named in this plan.
- Preserve layer direction: `core → widgets → viewer/archive/search → ui`.

---

## File Structure

### New client files

- `src/core/account/ClipboardHistoryStore.h/.cpp` — local history model, admission helpers, private persistence, image cache, deduplication and eviction.
- `tests/core/account/test_ClipboardHistoryStore.cpp` — persistence/admission/dedup/eviction/security tests.

### Modified client files

- `src/core/account/AccountClient.h/.cpp` — delivery metadata types; text/image send; pending list; streaming content download; ACK; progress signals.
- `src/core/account/DeviceAgent.h/.cpp` — emit incoming delivery id from `clipboard_delivery` WebSocket frames.
- `src/core/CMakeLists.txt` and `tests/core/CMakeLists.txt` — register store and tests.
- `src/ui/CloudClipboardController.h/.cpp` — local clipboard observer, history orchestration, explicit send and receive state.
- `src/ui/NotepadPanel.h/.cpp` — read-only history list and preview UI.
- `tests/ui/test_NotepadPanel.cpp` — panel interaction/controller tests.
- `resources/translations/ttc_*.ts/.qm` — new labels/status strings and removal of retired controls.

### Modified server files

- `server/src/db.rs` — delivery/payload tables and migration tests.
- `server/src/clipboard.rs` — new explicit send, pending list, content and ACK handlers.
- `server/src/agent.rs` — per-target `clipboard_delivery` notification.
- `server/src/lib.rs` — routes and body limits.
- `server/tests/api.rs` — account isolation, recipients, offline delivery, content, ACK, expiry and validation.

---

### Task 1: Local history domain and persistence

**Files:**
- Create: `src/core/account/ClipboardHistoryStore.h`
- Create: `src/core/account/ClipboardHistoryStore.cpp`
- Create: `tests/core/account/test_ClipboardHistoryStore.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `tests/core/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class ClipboardRecordOrigin { Local, Incoming };`
  - `enum class ClipboardRecordKind { Text, Image };`
  - `struct ClipboardHistoryRecord { QString id; ClipboardRecordOrigin origin; ClipboardRecordKind kind; QString text; QString imagePath; QString mime; QString sha256; QString sourceDeviceId; QString sourceDeviceName; qint64 size; int width; int height; QDateTime created; };`
  - `struct ClipboardCapture { ClipboardRecordKind kind; QString text; QImage image; QString mime; };`
  - `static bool ClipboardHistoryStore::captureFromMimeData(const QMimeData *, ClipboardCapture *);`
  - `bool load(); const QVector<ClipboardHistoryRecord> &records() const;`
  - `ClipboardHistoryRecord addLocalText(const QString &);`
  - `ClipboardHistoryRecord addLocalImage(const QByteArray &encoded, const QString &mime, int width, int height);`
  - `ClipboardHistoryRecord addIncomingText(...);`
  - `ClipboardHistoryRecord addIncomingImageFile(...);`
  - `bool remove(const QString &id); bool lookup(const QString &id, ClipboardHistoryRecord *) const;`

- [ ] **Step 1: Write MIME admission tests**

```cpp
TEST(ClipboardHistoryStoreTest, ImageWinsOverHtmlAndTextFallback) {
    QMimeData mime;
    mime.setImageData(QImage(8, 8, QImage::Format_ARGB32));
    mime.setHtml("<b>fallback</b>");
    mime.setText("fallback");
    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Image);
}

TEST(ClipboardHistoryStoreTest, RichTextStoresPlainTextOnly) {
    QMimeData mime;
    mime.setHtml("<b>Hello</b>");
    mime.setText("Hello");
    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(capture.text, QStringLiteral("Hello"));
}

TEST(ClipboardHistoryStoreTest, RejectsFilesAndPrivateFormats) {
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile("C:/secret.txt")});
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&files, nullptr));
    QMimeData privateMime;
    privateMime.setData("application/x-password", "secret");
    privateMime.setText("secret");
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&privateMime, nullptr));
}
```

- [ ] **Step 2: Run the focused tests and verify red**

Run:

```powershell
.\scripts\windows-dev-cmake.ps1 --build build/windows-msvc-debug --target core_tests
.\build\windows-msvc-debug\tests\core\core_tests.exe --gtest_filter="ClipboardHistoryStoreTest.*"
```

Expected: compile failure because the store/types do not exist.

- [ ] **Step 3: Implement admission and record serialization**

Use `QMimeData::hasImage()`/`imageData()` first, then `hasText()`/`text()`. Reuse the current private-MIME rules from `CloudClipboardController::isPrivateOrFileMime`. Encode images to PNG in the store and hash the exact stored bytes with SHA-256. Store manifest version `1` as JSON using `QSaveFile`; store image files under `<config>/cloud-clipboard/history/images/<id>.bin`. Call `PrivatePath::restrictDirectory/restrictFile`.

- [ ] **Step 4: Add persistence, deduplication and eviction tests**

```cpp
TEST(ClipboardHistoryStoreTest, PersistsFiftyNewestAndDeduplicates) {
    QTemporaryDir dir;
    ClipboardHistoryStore store(dir.path());
    for (int i = 0; i < 51; ++i)
        store.addLocalText(QString::number(i));
    store.addLocalText(QStringLiteral("50"));
    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 50);
    EXPECT_EQ(reloaded.records().first().text, QStringLiteral("50"));
    EXPECT_EQ(std::count_if(reloaded.records().cbegin(), reloaded.records().cend(),
                            [](const auto &r) { return r.text == "50"; }), 1);
}
```

Add image round-trip, corrupt-manifest quarantine, orphan-image cleanup, and raw-path privacy assertions.

- [ ] **Step 5: Run core store tests green**

Expected: all `ClipboardHistoryStoreTest.*` pass.

- [ ] **Step 6: Commit**

```bash
git add src/core/account/ClipboardHistoryStore.* src/core/CMakeLists.txt \
        tests/core/account/test_ClipboardHistoryStore.cpp tests/core/CMakeLists.txt
git commit -m "feat: add persistent local clipboard history"
```

---

### Task 2: Server delivery schema and text delivery API

**Files:**
- Modify: `server/src/db.rs`
- Modify: `server/src/clipboard.rs`
- Modify: `server/src/lib.rs`
- Modify: `server/tests/api.rs`

**Interfaces:**
- Produces routes:
  - `POST /v1/clipboard/send` with `Content-Type: text/plain; charset=utf-8` and raw UTF-8 body.
  - `GET /v1/clipboard/deliveries` returns `DeliveryList { deliveries: Vec<DeliveryView> }` for the authenticated device.
  - `GET /v1/clipboard/deliveries/{id}/content` returns raw payload bytes.
  - `POST /v1/clipboard/deliveries/{id}/ack` marks delivered.
- `DeliveryView`: `id`, `payload_id`, `kind`, `mime`, `size`, `width`, `height`, `sha256`, `source_device_id`, `source_device_name`, `created`, `expires`.

- [ ] **Step 1: Write migration tests**

Assert `Db::open()` creates/migrates:

```sql
CREATE TABLE clipboard_payloads (
  id TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL,
  source_device_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  mime TEXT NOT NULL,
  content BLOB NOT NULL,
  size INTEGER NOT NULL,
  width INTEGER,
  height INTEGER,
  sha256 TEXT NOT NULL,
  created TEXT NOT NULL,
  expires TEXT NOT NULL
);
CREATE TABLE clipboard_deliveries (
  id TEXT PRIMARY KEY,
  payload_id TEXT NOT NULL,
  target_device_id TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'pending',
  delivered_at TEXT,
  UNIQUE(payload_id, target_device_id)
);
```

- [ ] **Step 2: Write red API tests for all-device text send**

Register/login three devices under one account and one device under another. Send from device A. Assert two delivery rows for B/C, none for A/other account, text content is exact UTF-8, ACK removes the delivery from pending, and duplicate ACK is idempotent.

- [ ] **Step 3: Implement transactional payload/delivery creation**

Authenticate, read max 64 KiB body, query all account devices except sender, and inside one SQLite transaction insert payload plus delivery rows. If there are zero recipients, return `{ payload_id, recipient_count: 0 }` without retaining a payload.

- [ ] **Step 4: Implement pending/content/ACK and TTL cleanup**

Every route runs cleanup first. ACK verifies the target device owns the delivery. Delete payload after all deliveries are delivered; purge pending payloads after seven days.

- [ ] **Step 5: Run server tests green**

```bash
cd server && cargo test clipboard_delivery -- --nocapture
```

- [ ] **Step 6: Commit**

```bash
git add server/src/db.rs server/src/clipboard.rs server/src/lib.rs server/tests/api.rs
git commit -m "feat: queue explicit clipboard deliveries"
```

---

### Task 3: Server image upload and WebSocket notification

**Files:**
- Modify: `server/src/clipboard.rs`
- Modify: `server/src/agent.rs`
- Modify: `server/src/lib.rs`
- Modify: `server/tests/api.rs`

**Interfaces:**
- Extends `POST /v1/clipboard/send` for `Content-Type: image/*` raw body.
- Required headers: `X-Clipboard-Width`, `X-Clipboard-Height`, `X-Clipboard-Sha256`.
- Response: `{ payload_id, recipient_count }`.
- WebSocket frame: `{ "type":"clipboard_delivery", "delivery_id":"...", "kind":"image|text", "size":N }` sent only to online target devices after commit.

- [ ] **Step 1: Write red image/API notification tests**

Cover valid image, bad hash, >25 MiB, invalid dimensions, account isolation, online targets receiving exactly one notification, and sender receiving none.

- [ ] **Step 2: Implement binary body route and validation**

Set route body limit to `25 * 1024 * 1024`. Compute server SHA-256 and compare to header. Validate MIME/dimensions/pixels before transaction. Store the raw body BLOB.

- [ ] **Step 3: Add targeted delivery broadcaster**

Add `broadcast_clipboard_delivery(state, user_id, target_device_ids, delivery_metadata)` beside existing agent broadcasters. Do not broadcast payload bytes.

- [ ] **Step 4: Run server tests green and legacy tests unchanged**

```bash
cd server && cargo test clipboard -- --nocapture
```

- [ ] **Step 5: Commit**

```bash
git add server/src/clipboard.rs server/src/agent.rs server/src/lib.rs server/tests/api.rs
git commit -m "feat: cache image clipboard deliveries"
```

---

### Task 4: Client transport contract

**Files:**
- Modify: `src/core/account/AccountClient.h`
- Modify: `src/core/account/AccountClient.cpp`
- Modify: `src/core/account/DeviceAgent.h`
- Modify: `src/core/account/DeviceAgent.cpp`
- Test: `tests/core/account/test_AccountClient.cpp`

**Interfaces:**
- Produces:
  - `struct ClipboardDeliveryInfo` matching server metadata.
  - `void sendClipboardText(const QString &text);`
  - `void sendClipboardImageFile(const QString &filePath, const QString &mime, int width, int height, const QByteArray &sha256);`
  - `void fetchClipboardDeliveries();`
  - `void downloadClipboardDelivery(const ClipboardDeliveryInfo &, const QString &destinationPartPath);`
  - `void acknowledgeClipboardDelivery(const QString &deliveryId);`
- Signals:
  - `clipboardSendProgress(qint64 sent, qint64 total)`
  - `clipboardSendFinished(QString payloadId, int recipientCount)`
  - `clipboardDeliveriesReady(QVector<ClipboardDeliveryInfo>)`
  - `clipboardDownloadProgress(QString id, qint64 received, qint64 total)`
  - `clipboardDownloadFinished(QString id, QString partPath)`
  - `clipboardDeliveryAcknowledged(QString id)`
  - `DeviceAgent::clipboardDeliveryAvailable(QString deliveryId)`

- [ ] **Step 1: Write red request/response tests**

Extend the mock HTTP server to inspect raw body and headers. Assert text body, image streaming headers, upload progress, pending parsing, content streaming and ACK path.

- [ ] **Step 2: Implement text and image sends**

Use the existing authenticated request builder. For image, parent a `QFile` to the `QNetworkReply`, call `QNetworkAccessManager::post(request, file)`, and forward `uploadProgress`. Do not Base64 the image.

- [ ] **Step 3: Implement streaming download**

Write reply chunks to `QSaveFile`/`.part`, enforce expected size during `readyRead`, hash incrementally with `QCryptographicHash`, commit only after matching digest, then emit completion.

- [ ] **Step 4: Implement pending/ACK and WebSocket frame parsing**

`DeviceAgent::onTextMessage` emits `clipboardDeliveryAvailable` for `clipboard_delivery`. AccountClient pending and ACK calls use the new routes.

- [ ] **Step 5: Run focused core tests green**

```powershell
.\build\windows-msvc-debug\tests\core\core_tests.exe --gtest_filter="AccountClientTest.*Clipboard*"
```

- [ ] **Step 6: Commit**

```bash
git add src/core/account/AccountClient.* src/core/account/DeviceAgent.* \
        tests/core/account/test_AccountClient.cpp
git commit -m "feat: stream clipboard deliveries on the client"
```

---

### Task 5: Controller uses local history and delivery APIs

**Files:**
- Modify: `src/ui/CloudClipboardController.h`
- Modify: `src/ui/CloudClipboardController.cpp`
- Modify: `src/ui/CMakeLists.txt`
- Test: `tests/ui/test_NotepadPanel.cpp`

**Interfaces:**
- Consumes `ClipboardHistoryStore`, `ClipboardDeliveryInfo` and AccountClient delivery signals.
- Produces `const QVector<ClipboardHistoryRecord> &items() const` plus:
  - `select/copy/send/remove` operations by record id.
  - progress/state signals for panel.

- [ ] **Step 1: Replace controller tests with new-flow red tests**

Test automatic local capture, feedback-loop suppression after double-click copy, explicit local-only sending, incoming pending persistence, and no old automatic publish calls.

- [ ] **Step 2: Implement clipboard capture**

On `QClipboard::dataChanged`, capture once through the store. Image encoding/storage runs through `QtConcurrent`; serialize store writes so two changes cannot race the manifest.

- [ ] **Step 3: Implement explicit send**

Look up selected local record. Text calls `sendClipboardText`; image sends the cached file. Expose encode/upload/wait status and progress. Zero recipients emits “No other devices online or registered” without a server payload.

- [ ] **Step 4: Implement incoming receive orchestration**

Fetch pending on login and agent ready/notification. Text enters history then ACKs. Image streams to store staging path, validates in AccountClient, moves into history, then ACKs. Never call `QClipboard::set*` here except from explicit `copyRecordToClipboard`.

- [ ] **Step 5: Run focused UI/controller tests green**

```powershell
.\scripts\windows-dev-cmake.ps1 --build build/windows-msvc-debug --target ui_tests
.\build\windows-msvc-debug\tests\ui\ui_tests.exe --gtest_filter="CloudClipboardControllerTest.*"
```

Expected: all controller tests pass and no server publish is emitted for automatic local captures.

- [ ] **Step 6: Commit**

```bash
git add src/ui/CloudClipboardController.* src/ui/CMakeLists.txt tests/ui/test_NotepadPanel.cpp
git commit -m "feat: orchestrate explicit clipboard history delivery"
```

---

### Task 6: Redesign the history/preview panel

**Files:**
- Modify: `src/ui/NotepadPanel.h`
- Modify: `src/ui/NotepadPanel.cpp`
- Modify: `tests/ui/test_NotepadPanel.cpp`
- Modify: `resources/translations/ttc_*.ts/.qm`

**Interfaces:**
- Consumes controller records/actions/progress.
- UI object names for tests:
  - `CloudClipboardList`, `CloudClipboardSearch`, `CloudClipboardPreview`, `CloudClipboardSendButton`, `CloudClipboardCopyButton`, `CloudClipboardProgress`, `CloudClipboardStatus`.

- [ ] **Step 1: Write red UI tests**

Assert no editable input and no auto-upload/receive checkboxes; single-click previews; double-click copies; send button visible only for local records; incoming record never auto-writes clipboard; progress/status update from controller signals.

- [ ] **Step 2: Replace editor pane with read-only preview**

Delete `CloudClipboardEditor`, send-composition logic and auto-sync controls. Use read-only `QPlainTextEdit` for text and scaled `QLabel` for images in a `QStackedWidget`.

- [ ] **Step 3: Wire list behavior**

Current-item change updates preview. `itemDoubleClicked` calls controller copy. Send button calls explicit send for local item. Keep search/delete/clear-local-history actions.

- [ ] **Step 4: Wire transfer progress and translations**

Display bytes and percentage for upload/download. Add all new strings to nine catalogs and run `release-translations`; require zero unfinished entries.

- [ ] **Step 5: Run panel tests green**

```powershell
.\build\windows-msvc-debug\tests\ui\ui_tests.exe --gtest_filter="CloudClipboardPanelTest.*"
```

Expected: all panel interaction/progress tests pass in one process.

- [ ] **Step 6: Commit**

```bash
git add src/ui/NotepadPanel.* tests/ui/test_NotepadPanel.cpp resources/translations
git commit -m "feat: turn cloud clipboard into a history preview"
```

---

### Task 7: Retire old automatic/peer clipboard paths

**Files:**
- Modify: `src/ui/CloudClipboardController.h/.cpp`
- Modify: `src/core/account/AccountClient.h/.cpp`
- Modify: `src/core/account/DeviceAgent.h/.cpp`
- Modify: `src/core/account/FileShareServer.h/.cpp`
- Modify: `src/core/config/Settings.h/.cpp`
- Modify: `tests/core/account/test_AccountClient.cpp`
- Modify: `tests/core/account/test_FileShareServer.cpp`
- Modify: `tests/ui/test_NotepadPanel.cpp`

**Interfaces:**
- Removes new UI/controller usage of old `clipboardReady`, `clipboardPublished`, `clipboardSessionReady`, auto-upload/auto-receive and clipboard ticket APIs.
- Legacy server routes remain; client code can retain deprecated methods only if another compatibility caller still uses them.

- [ ] **Step 1: Add tests proving old automatic flow is unreachable**

No OS clipboard change triggers a server publish. No incoming server event writes directly to QClipboard. FileShareServer does not mint clipboard-scoped tickets for the new flow.

- [ ] **Step 2: Remove obsolete UI/settings and peer-transfer wiring**

Delete old controller state/methods and configuration menu/UI references. Remove FileShareServer clipboard serving only after `git grep` proves no caller remains.

- [ ] **Step 3: Run account/core/UI focused suites**

- [ ] **Step 4: Commit**

```bash
git add src/ui/CloudClipboardController.* src/core/account src/core/config tests
git commit -m "refactor: retire automatic cloud clipboard mirroring"
```

---

### Task 8: Integration, review and Windows test package

**Files:**
- Review all files above.
- Update: `server/README.md` with delivery routes/retention.
- Update translations if integration adds final status strings.

- [ ] **Step 1: Run server suites**

```bash
cd server && cargo test
```

- [ ] **Step 2: Build affected Windows targets**

```powershell
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
.\scripts\windows-dev-cmake.ps1 --build build/windows-msvc-debug --target core_tests ui_tests
```

Run focused tests in separate processes to avoid the known Windows offscreen full-suite activation crash.

- [ ] **Step 3: Security/data-loss review**

Verify account isolation, path/ID validation, body limits, atomic files, no tokens in URLs/logs, no automatic clipboard overwrite, ACK only after hash validation, and payload cleanup.

- [ ] **Step 4: Request code review**

Use `superpowers:requesting-code-review`; apply only verified findings.

- [ ] **Step 5: Build and launch a Release test binary**

Create a fresh Release build directory, build `FileCommander`, obtain permission to overwrite `dist/FileCommander-windows-x64/FileCommander.exe`, and launch it. Smoke test local capture, image-first MIME, single/double click, all-device send, offline reconnect, upload/download progress and server cleanup.

- [ ] **Step 6: Commit integration docs/fixes**

```bash
git add server/README.md resources/translations src tests server
git commit -m "feat: deliver explicit cloud clipboard history"
```
