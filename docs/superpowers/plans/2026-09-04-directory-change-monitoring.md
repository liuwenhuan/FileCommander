# Directory Change Monitoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:executing-plans` to execute this plan task by task.

**Goal:** Keep visible directory listings current after external filesystem
changes on Windows and Linux, while retaining a refresh fallback for remote
providers and unsupported notification backends.

**Architecture:** A small Qt `DirectoryChangeMonitor` owns one native watch for
the model's current local directory. Windows uses an overlapped
`ReadDirectoryChangesW` handle observed by `QWinEventNotifier`; Linux uses a
nonblocking `inotify` descriptor observed by `QSocketNotifier`. The monitor
reports coalesced change/reconciliation signals. `FileSystemModel` debounces
those signals and reuses its asynchronous `list()` path. `FilePanel` preserves
selection around external reloads and `MainWindow` requests activation refreshes.

**Tech Stack:** Qt 5, C++17, `ReadDirectoryChangesW`, `inotify`,
`QFutureWatcher`, `QTimer`, GoogleTest/QtTest.

**Spec:** `docs/superpowers/specs/2026-09-04-directory-change-monitoring.md`

## Task 1: Add monitor API and regression tests

**Files:** `src/core/filesystem/DirectoryChangeMonitor.h`,
`src/core/filesystem/DirectoryChangeMonitor.cpp`,
`tests/core/filesystem/test_DirectoryChangeMonitor.cpp`,
`src/core/CMakeLists.txt`, `tests/core/CMakeLists.txt`

Define the platform-neutral monitor state and signals, then add tests for
temporary-directory events, setup failure, and stopping a watch. Run the new
tests before implementing platform backends so the initial test failure is
recorded.

## Task 2: Implement native platform backends

**Files:** `src/core/filesystem/DirectoryChangeMonitor_windows.cpp`,
`src/core/filesystem/DirectoryChangeMonitor_linux.cpp`,
`src/core/filesystem/DirectoryChangeMonitor_stub.cpp`

Implement nonrecursive directory watching, rename pairing, event parsing,
overflow/invalidation detection, and clean shutdown. The stub reports an
unavailable watcher and relies on reconciliation.

## Task 3: Connect the monitor to `FileSystemModel`

**Files:** `src/core/filesystem/FileSystemModel.h`,
`src/core/filesystem/FileSystemModel.cpp`,
`tests/core/filesystem/test_FileSystemModel.cpp`

Start and stop the watch with provider/path changes. Debounce native events,
discard events from stale paths, and trigger a background refresh while keeping
the current model contract and generation guards intact. Expose whether a
healthy local watch exists and provide an activation/operation refresh hook.

## Task 4: Preserve panel state and refresh on activation

**Files:** `src/ui/FilePanel.h`, `src/ui/FilePanel.cpp`,
`src/ui/MainWindow.h`, `src/ui/MainWindow.cpp`,
`tests/ui/test_ExternalDirectoryRefresh.cpp`

Preserve selected/current rows when an external refresh is requested. On window
activation, refresh remote or unreconciled listings only; healthy native local
watchers remain event-driven. Ensure computer/flat/archive views are excluded.

## Task 5: Add operation freshness preflight

**Files:** `src/core/filesystem/FileSystemModel.h`,
`src/ui/MainWindow.cpp`, focused operation tests

Before queueing a local copy/move/delete/rename, consume a pending
reconciliation request and refresh the affected panel when practical. Keep the
actual operation authoritative: all opens, writes, and deletes still report
their real error state from the worker, so a race cannot become a false success.

## Task 6: Verify and package

Run focused core/UI tests, the complete available test target, Windows build and
package smoke. On Linux, configure/build the core test target when the local
toolchain is available. Record unsupported-platform fallback behaviour and any
remaining machine-dependent checks.

