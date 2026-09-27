# Confirmed Device Transfer Progress

## Goal

Both devices show a non-modal, minimizable progress window during an interactive device-to-device file transfer. The progress bar represents bytes successfully written by the receiving FileShareServer, not bytes merely handed to the sender's local relay socket. Sending and receiving rates are measured over recent byte deltas, so a stalled connection falls to zero. A headless background send remains headless.

## Protocol

Each PUT to a FileCommander peer carries a random transfer ID in `X-FileCommander-Transfer-Id`. A receiver that supports acknowledgements advertises `X-FileCommander-Upload-Progress: v1` in its authenticated WebDAV response. The receiver maintains bounded, short-lived status for that ID, scoped to the ticket that authorized the PUT. An authenticated GET of `/.filecommander/upload-progress/<id>` returns JSON containing `written`, `total`, and `state` (`receiving`, `complete`, `failed`, or `interrupted`). `written` includes an existing resume prefix. Only successful writes to the staging file advance the counter; complete is reported only after the staged file has been committed. Status disappears after a bounded retention period.

The sender polls over a separate authenticated TLS connection, at most four times a second, while the PUT is active and through the final commit wait. A failed or delayed poll never invents progress; the last confirmed count remains visible, with the confirmed rate decaying to zero. Older peers without the advertised capability continue to transfer unchanged, but the UI labels receive confirmation unavailable rather than presenting local socket bytes as receiver progress. The upload cannot be marked successful until the normal WebDAV PUT response confirms commit.

## Presentation

On the sender, the progress bar and confirmed byte count use receiver acknowledgements. Sending rate uses libcurl's upload counter; receiving rate uses successive acknowledged counts. On the receiver, the same written count drives its bar and receiving rate. Both rates use a short rolling interval and update on a timer so they fall to zero during stalls. Polling latency means the displays need not match at the same millisecond; the sender must never exceed the last receiver-confirmed position.

The two progress windows are ordinary non-modal windows with a minimize button and taskbar/Alt-Tab restoration. Starting an interactive transfer may show its window without activation; later progress never raises or reactivates it. Minimizing, switching apps, or closing a progress window does not pause or abort the transfer. An explicit Abort control keeps its current behavior on the sender. Decision dialogs for conflicts and errors remain modal only while a decision is needed. Headless CLI transfer emits no progress windows.

## Safety And Compatibility

Ticket validation, TLS pinning, path confinement, per-path upload locking, staged files, resume prefix verification, cancellation, and final commit semantics remain intact. Status IDs cannot disclose other tickets' progress. Progress records have count and time bounds. A resumed transfer begins at its verified receiver offset. Unknown totals remain indeterminate. Receiver write/flush/rename errors leave a resumable partial and produce failure state, not 100% success. A relay disconnect freezes confirmed progress and produces an error after the normal transfer timeout.

## Verification

Unit tests cover status authentication/isolation, monotonic written counts, resume offsets, terminal states, stale records, rate decay, minimization/focus behavior, and old-peer fallback. A local TLS+relay integration test transfers a multi-megabyte file with deliberate receiver lag and proves the sender's displayed count never leads the receiver's written count. Windows and Linux builds and relevant UI tests must pass.
