# Directory Change Monitoring Specification

## Goal

Keep the currently displayed local directory accurate after another process
creates, removes, renames, or modifies an entry, without adding a resident
service and without blocking the UI thread.

## Scope

- Windows local directories use `ReadDirectoryChangesW`.
- Linux local directories use `inotify`.
- Network providers, archives, and unsupported platforms use an activation-time
  refresh when their listing is visible.
- Watch events are coalesced and cause a background re-list of the current
  directory. The existing selection-restoration path is preserved.
- Watch buffer overflow, invalidation, directory replacement, and watch setup
  failure mark the listing as needing reconciliation instead of pretending the
  event stream is complete.
- File operations retain their existing per-operation error handling; a cheap
  model freshness check may request a refresh before an operation is queued,
  but no synchronous recursive scan is introduced.

## Non-goals

- No recursive watcher for every child directory.
- No Windows service, daemon, database, or filesystem index.
- No polling loop for healthy native watchers.
- No change to archive contents or remote server notification semantics.

## Behaviour

The monitor watches only the directory represented by the active model. Events
are delivered on the Qt GUI event loop, coalesced for a short interval, and
trigger one asynchronous provider listing. A monitor event never mutates model
rows directly, which keeps sorting, filtering, parent entries, and provider
specific metadata in one code path.

On activation, a model with a healthy local watcher does nothing. A model whose
watcher is unavailable or requires reconciliation refreshes once. Remote
sessions refresh on activation because the network protocols do not provide a
portable directory notification contract.

## Test contract

- A native monitor reports create/remove/rename activity for a temporary
  directory on the current platform.
- A model reloads after an external create and exposes the new row.
- Repeated events are coalesced into one reload window.
- A stale watcher cannot refresh a newly navigated directory.
- Remote/flat listings never start a local native watcher.
- Watch setup failure requests reconciliation.
