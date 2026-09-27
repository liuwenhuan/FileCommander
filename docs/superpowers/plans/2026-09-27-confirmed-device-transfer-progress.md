# Confirmed Device Transfer Progress Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show receiver-confirmed device transfer progress and recent sending/receiving rates in minimizable, non-modal windows on both machines.

**Architecture:** Extend the existing ticket-authenticated WebDAV connection with per-PUT progress IDs and a bounded status endpoint. The provider polls receiver-confirmed bytes while the upload runs, forwarding those measurements through FileOperations and OperationQueue to the sender UI. FileShareServer emits the same measurements for the receiver UI.

**Tech Stack:** C++17, Qt 5 Widgets/Network, libcurl, Google Test, CMake.

**Spec:** `docs/superpowers/specs/2026-09-27-confirmed-device-transfer-progress-design.md`

## Global Constraints

- No new transport service or dependency; preserve TLS pinning and ticket authentication.
- Progress means successful receiver staging-file writes; completion means successful final commit.
- Polling at most four times a second; records bounded by count and lifetime.
- Keep old peers functional, but never label their local send counter as received.
- Interactive progress windows are minimizable, non-modal, and never raised on update; headless sends stay hidden.
- Preserve existing resume, conflict, cancel, and error behavior.

## Review Focus

- Existing destination overwritten while a partial is active: status must identify the current PUT, not the old final file. Test in Task 1.
- Interrupted resumable upload: initial confirmed count is the verified offset, not zero or double-counted. Test in Task 2.
- Lost status request: freeze confirmed progress and decay rate to zero without false completion. Test in Task 3.
- Minimized windows and app switching: updates cannot restore or activate either window. Test in Task 4.
- Older peer capability absent: transfer remains usable and receive progress is explicitly unavailable. Test in Task 2.

---

### Task 1: Receiver Progress Status

**Files:** Modify `src/core/account/FileShareServer.cpp`, `src/core/account/FileShareServer.h`; test `tests/core/account/test_FileShareServer.cpp`.

**Interfaces:** Produce `FileShareServer::uploadProgress(QString id, QString path, qint64 written, qint64 total, QString state)` and the authenticated `GET /.filecommander/upload-progress/<id>` JSON response. Advertise `X-FileCommander-Upload-Progress: 1`.

- [ ] Add failing tests for accepted writes, overwrites, resume offsets, ticket isolation, failure/disconnect, and status expiration.
- [ ] Run `core_tests --gtest_filter=FileShareServerTest.*` and observe expected failures.
- [ ] Implement bounded progress registry and signals on the existing server thread.
- [ ] Run receiver tests until green.

### Task 2: Provider Confirmation And Fallback

**Files:** Modify `src/core/network/CurlWebDavProvider.cpp`, `src/core/network/CurlWebDavProvider.h`, `src/core/filesystem/FileProvider.h`; test `tests/core/account/test_FileShareServer.cpp`, `tests/core/account/test_RelayTunnel.cpp`.

**Interfaces:** Produce `FileHandle::receiverConfirmedBytes()` (default `-1`), `FileHandle::receiverConfirmationAvailable()` (default false), and a provider close path that continues reporting confirmations during the final PUT wait.

- [ ] Add failing provider tests for advertised capability, live confirmation, resume offset, and old-peer fallback.
- [ ] Run focused tests and observe expected failures.
- [ ] Add transfer IDs and authenticated, bounded polling on a separate control connection.
- [ ] Run focused provider and relay tests until green.

### Task 3: Send Progress And Rates

**Files:** Modify `src/core/operations/FileOperations.cpp`, `src/core/operations/FileOperations.h`, `src/core/operations/OperationQueue.cpp`, `src/core/operations/OperationQueue.h`, `src/ui/dialogs/TransferProgressDialog.cpp`, `src/ui/dialogs/TransferProgressDialog.h`; test `tests/core/operations/test_FileOperations.cpp`, `tests/ui/test_OperationProgressMotion.cpp`.

**Interfaces:** Add a device-transfer metrics signal carrying locally sent and receiver-confirmed cumulative bytes. The normal queue progress signal uses confirmed bytes when supported.

- [ ] Add failing tests for no local-byte lead, rate decay during stalls, final tail, and explicit unavailable fallback.
- [ ] Run focused core/UI tests and observe expected failures.
- [ ] Forward confirmation measurements, use rolling-rate samples, and keep normal provider progress unchanged.
- [ ] Run focused tests until green.

### Task 4: Receiver And Window Behavior

**Files:** Create `src/ui/dialogs/IncomingTransferWindow.h/.cpp`; modify `src/ui/MainWindow.cpp`, `src/ui/dialogs/TransferProgressDialog.cpp`, `src/widgets/DialogTitleBar.cpp/.h`, `src/ui/CMakeLists.txt`; test `tests/ui/test_TransferProgressLayout.cpp` and new receiver-window tests.

**Interfaces:** MainWindow consumes `FileShareServer::uploadProgress`; both windows are taskbar-restorable, minimizable, non-modal, and remain minimized on updates.

- [ ] Add failing tests for receiver display and minimize/no-activation behavior.
- [ ] Run focused UI tests and observe expected failures.
- [ ] Implement the receiver window and adjust sender progress chrome without changing headless CLI behavior.
- [ ] Run focused UI tests until green.

### Task 5: Integration And Languages

**Files:** Modify `tests/core/account/test_RelayTunnel.cpp`, translation sources under `resources/i18n`, and relevant build metadata only if required.

- [ ] Add a delayed-receiver relay test asserting confirmed sender progress never leads receiver writes and both complete at the same total.
- [ ] Run it red, then address the identified integration gap.
- [ ] Update translations for new visible strings.
- [ ] Build Windows/Linux targets, run focused suites, and inspect a Windows UI smoke test for minimization and focus.
