# Why the custom providers exist instead of gvfs (measured, 2026-07)

This question comes up naturally — gvfs already speaks SMB/SFTP/FTP/WebDAV, and its FUSE mount exposes a real local path any program can open. It was measured end-to-end against real servers before deciding to keep the custom stack. Re-read this before proposing to replace `src/core/network/` with gvfs.

**Directory listing through the gvfs FUSE mount is unusable on anything but a LAN.** Same server, same 1408-entry directory:

| path | time |
|---|---|
| own `SmbProvider` (`smbc_readdirplus2`) | 88–142 ms |
| `smbclient` at the protocol level | 115 ms |
| `gio list -l` (GIO API → D-Bus → `gvfsd-smb`) | 100 ms |
| **through the FUSE mount** | **1347–1416 ms** |

The penalty is *not* gvfs's SMB backend: `gvfsd-smb` imports `smbc_getFunctionReaddirPlus2` and never references plain `readdir` — it batches exactly like we do. The loss happens in `gvfsd-fuse`, which does **not** implement the FUSE readdirplus operation (zero `readdirplus` references in the binary) even though kernel 6.6 and libfuse3 both support it. So the kernel falls back to "readdir for names, then one `getattr` per entry", and the backend's batching is thrown away at the bridge.

That cost is one round trip per entry, so it scales with latency. On a 120 ms-RTT host, SFTP measured **139.7 ms per entry** — a 1408-entry directory extrapolates to **3m17s**. (WebDAV is the exception: `gvfsd-dav` pre-fills directory attributes, so it stays fast — 9 ms warm.) FUSE's attribute cache does work once populated (a second pass over 200 entries costs 0 ms); it is only the first fill that pays.

We cannot fix any of this from the app: `gvfsd-fuse` is started by the session with default mount options, so `attr_timeout`/`entry_timeout` are not ours to set. Upstream knows ([rhbz#1478411](https://bugzilla.redhat.com/show_bug.cgi?id=1478411), [rhbz#1569868](https://bugzilla.redhat.com/show_bug.cgi?id=1569868)).

**Where gvfs *is* the right tool: handing one file to an external program.** Our providers live inside this process, so VLC/an editor/any `%f`-only `.desktop` handler cannot reach them — the only alternatives are a downloaded copy (slow, and read-only, so edits are silently lost) or a protocol URL (this machine's FFmpeg has no `smb` protocol at all). A gvfs mount point gives a real read-write path, and single-file access never touches the readdirplus path: measured 85 ms to open, 202 ms to seek to 50% and decode a frame, 4 ms for a same-share cross-directory rename. `GvfsMounter::localPathForUri()` already exists for exactly this and is currently unused.

**Bulk transfer is a different question from listing, and the answer is narrower.** Copy/move/delete/mkdir on a network tab go through the custom provider (`OperationQueue::enqueueProviderCopy` / `…Move` / `…Mkdir`); gvfs is not in that path at all. Measured against the same two servers, comparing the FUSE mount with the library each provider actually links (libsmbclient for SMB, libssh2 for SFTP):

| | gvfs FUSE | provider (direct) | |
|---|---|---|---|
| **LAN SMB, gigabit, 400 MB** | | | |
| write | 103 MB/s | 107.5 MB/s | −4% |
| read | 87.6 MB/s | 107.9 MB/s | −19% |
| same-share cross-dir move, warm | 4.4–5.5 ms | 1.0–1.3 ms | 4–5× |
| **WAN SFTP, 117 ms RTT, 20 MB** | | | |
| upload | 193 kB/s | 1104 kB/s | **5.7×** |
| download | 464 kB/s | 2415 kB/s | **5.2×** |

Two things this rules out as arguments:

- **gvfs does do server-side move.** `mv` inside one mount point relocates a 400 MB file in ~5 ms — `gvfsd-smb` passes the rename through. The provider is faster in relative terms only because its connection is already warm and gvfs adds a FUSE hop; on a single move nobody can tell. (Measure rename on a *warm* connection: a fresh `smbc_init` + connect + negotiate + auth costs ~33 ms and swamps the ~1 ms rename itself.)
- **On a LAN, FUSE is not the throughput bottleneck.** Write is within 4%, i.e. the wire is saturated either way.

What survives is latency: FUSE requests are synchronous and unpipelined, so every 128 KB block costs a round trip, and at 117 ms RTT that is a 5–6× loss. Note this is a *much* gentler penalty than listing suffers (one round trip per 128 KB rather than per directory entry) — it does not extrapolate to the 3m17s figure above. So the case for keeping transfers on the providers rests on two things only: high-latency links, and the fact that `OperationQueue`'s resume, progress reporting, overwrite prompts and retry are all built on the provider byte stream. If a deployment is LAN-only, moving transfers to gvfs to simplify the code is a defensible trade.

If the two stacks are ever unified, the direction is the **GIO API**, not the FUSE mount — it measured 100 ms above. The cost is a compile-time glib/gio dependency (`GvfsMounter.h` documents the deliberate choice to shell out to the `gio` CLI and keep that surface at zero), a hard runtime requirement on `gvfs-backends`, and giving up the connection pooling, `maxReadChannels()` concurrency control, libsecret credential path, and the `MpvStreamSource` read/seek callbacks.
