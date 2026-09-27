# Three-way relay upload for large local files

## Intent

Upload a local file larger than 20 MiB over the FileCommander account relay
through three concurrent connections. A direct LAN connection, a smaller file,
a rate-limited transfer, a pre-existing single-stream partial, or a peer without
the new capability keeps the current single-PUT behavior. The remote final file
must never expose incomplete content.

## Wire contract

The receiving peer advertises `X-FileCommander-Multipart: v1` during the normal
capability probe. Every request uses the existing pinned end-to-end TLS and
short-lived device ticket. `POST /.filecommander/multipart` accepts a JSON
destination path, total size, and source SHA-256 and returns an opaque transfer
ID with three persisted per-part offsets. The three fixed ranges divide the
file as evenly as possible, assigning one extra byte to the first
`size % 3` parts.
Each part is uploaded independently to `PUT /.filecommander/multipart/<id>/<n>`
with `X-FileCommander-Part-Offset` and its exact remaining `Content-Length`.
`GET /.filecommander/multipart/<id>` reports receiver-confirmed offsets.
`POST /.filecommander/multipart/<id>/commit` verifies the assembled source hash
and publishes the final file atomically. Interrupted parts remain resumable.

## Accounting and fallback

Three connections read disjoint ranges from the same local file without
buffering it all in memory. Sending speed is the aggregate of their actual
wire progress. Receiving speed and the main progress bar use the peer's
persisted-byte acknowledgements. Cancel and pause affect all connections;
once final commit starts, the sender waits for the receiver's definitive
reply because disconnecting that request cannot undo an atomic publication.
A SHA-256 mismatch discards staged parts so retrying the same source starts
cleanly. Commit success alone marks the task complete. A new sender paired with an old
receiver uses the existing single PUT and resume path.

## Verification

Focused tests cover the 20 MiB boundary, old single-stream partial resume,
three-way content and hash integrity, interruption/resume, unsupported peers,
and relay-only gating. An opt-in benchmark sends the user-specified 41 MiB
installer through the same simulated relay once per mode and verifies the
received SHA-256. Those timings measure the local test relay, not the user's
real inter-device WAN path.

## Local benchmark result

The specified `pixeloffice-windows-x64-setup.exe` was 42,008,302 bytes. Three
repeat runs through the in-process relay, excluding fixture setup and final
SHA-256 verification, had median transfer times of 202 ms (198 MiB/s) for
one PUT and 944 ms (42 MiB/s) for three parts. Each received file matched the
source SHA-256. This loopback result shows that multipart overhead dominates
when one connection already saturates a very fast local path; it does not
predict whether three channels help on a latency- or connection-limited WAN.
A final rerun after commit-handling fixes measured 188 ms (213 MiB/s) for one
PUT and 951 ms (42 MiB/s) for three parts. Testing actual inter-device WAN
speed requires an updated receiving peer that advertises multipart support.

## Live relay attempt to deepin-LGPC

On 2026-09-27, a headless send of the same 42,008,302-byte installer copy
connected through the actual account relay with single-stream forced. It
failed after 289,211 ms with a connection-lost error; the last sender-side
progress sample was 16,793,600 bytes. The old peer did not provide a
receiver-confirmed byte count, so that sample is not a measured receive size.
A second send with the override removed also negotiated `selected=single`:
deepin-LGPC did not advertise `X-FileCommander-Multipart: v1`. That redundant
single-stream send was stopped. A valid live three-way comparison needs the
receiving device to run a build containing the new multipart server.
