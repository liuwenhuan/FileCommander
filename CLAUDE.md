# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

FileCommander (文件指挥官) is a Qt5/C++17 dual-pane file manager for Linux (Debian/Deepin/UOS-focused). It supports local files, archives (7z/zip/tar/squashfs/UDF), and network backends (SFTP, FTP, WebDAV, SMB), with thumbnail previews for images/video, a quick-view pane (image/PDF/video/office docs), directory sync/compare, and an update checker (it announces releases; it does not install them).

The project was renamed from **ttc** to **FileCommander**; the rename is cosmetic-only debt. The built binary, desktop file, and install paths are all `FileCommander`, but internal identifiers still say `ttc`/`TTC_*`: the version header macro `TTC_VERSION`, CMake options `TTC_BUILD_TESTS`/`TTC_BUILD_BENCH`, the `ui_tests` compile define `TTC_SOURCE_DIR`, and the translation file prefix `ttc_<lang>.ts`/`.qm`. Don't "fix" these to match the product name in unrelated commits — treat them as the existing convention.

## Build & Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # Debug is the default if unset; packaging always uses Release
cmake --build build -j$(nproc)
./build/FileCommander
```

Build dependencies (Debian/Deepin package names): `qtbase5-dev qtbase5-dev-tools libqt5x11extras5-dev qttools5-dev-tools libarchive-dev libssh2-1-dev libsecret-1-dev libcurl4-openssl-dev libssl-dev libsmbclient-dev libmpv-dev libpoppler-qt5-dev zlib1g-dev libxcb1-dev`, plus `libgtest-dev` for the test suite (source-only package; tests build it via `add_subdirectory(/usr/src/googletest ...)`).

Office document preview (docx/pptx/xlsx, and the planned WPS formats) shells out to an external CLI, `office-oxide` (a separate codework fork project, not vendored here) — resolved at runtime via `OfficeConverter::resolveBinary` from `PATH`, then `~/.local/bin`, then `~/.cargo/bin`. Without it, Office preview silently does nothing.

### Tests

Tests build by default (`-DTTC_BUILD_TESTS=ON`) alongside the main app; packaging scripts turn them off.

```bash
cmake --build build --target core_tests ui_tests archive_tests viewer_tests
ctest --test-dir build                       # all suites
ctest --test-dir build -R core_tests         # one suite
./build/tests/core/core_tests --gtest_filter=SyncScannerTest.*   # one test, direct gtest binary
```

Each of `core`, `ui`, `archive`, `viewer` has its own gtest binary (`tests/<module>/CMakeLists.txt`) rather than one combined suite. `ui_tests` links a real `QApplication` (`test_main.cpp` supplies `main()`) since most UI tests instantiate real widgets/models rather than mocking Qt.

### Benchmarks

`bench/bench_dirlisting` (built when `TTC_BUILD_BENCH=ON`, the default) — currently the only benchmark, timing directory-listing performance against `core`.

### Packaging

```bash
packaging/build-deb.sh          # → dist/FileCommander_<ver>_amd64.deb
packaging/build-appimage.sh     # → dist/FileCommander-<ver>-x86_64.AppImage
```

The version is single-sourced from `project(FileCommander VERSION X.Y.Z)` in `CMakeLists.txt` — nowhere else needs editing. Things the packaging scripts know that are not obvious from reading them: runtime dependencies are generated rather than listed; `p7zip-full` is required and `p7zip` is not (only the full `7z` reads UDF); the AppImage falls back from Deepin's `dxcb` platform plugin to `xcb`; and a launcher started from an AppImage inherits that image's environment, which breaks `ffmpeg`/`gio`/terminal children — see `packaging/apprun-hook.sh`. The self-update release flow lives in `packaging/` and `src/core/update/`.

### Translations

```bash
cmake --build build --target update-translations   # lupdate: refresh ttc_<lang>.ts from source strings
# edit resources/translations/ttc_<lang>.ts in Qt Linguist
cmake --build build --target release-translations  # lrelease: compile .ts -> .qm, bundled via resources.qrc
```
A `.qm` dropped into `~/.config/FileCommander/translations/` overrides the bundled catalog without a rebuild.

## Architecture

CMake subdirectories are separate static libraries with a strict dependency direction — lower layers never depend on higher ones:

```
core  →  widgets  →  viewer, archive, search  →  ui  →  FileCommander (main.cpp)
```

- **`src/core`** — no UI dependency beyond Qt Widgets types used as data (e.g. icons in `FileInfo`). Filesystem abstraction (`filesystem/`), file ops (`operations/`), settings/session persistence (`config/`), directory sync/compare (`sync/`, `compare/`), device mount monitoring (`devices/`), the self-updater (`update/`), and all network backends (`network/`).
- **`src/widgets`** — tiny, dependency-free shared chrome (frameless dialog base, title bar, seek slider) so `ui`, `archive`, and `search` can all use the same look without depending on each other.
- **`src/viewer`** — text editor, image viewer, and `OfficeConverter` (the `office-oxide` subprocess wrapper).
- **`src/archive`** — archive browsing/compression via libarchive, plus a vendored public-domain LZMA SDK (`lzma_sdk/`, built as the internal `lzma7z` target) for encrypted/solid 7z and UDF images that libarchive can't read alone.
- **`src/search`** — file content/name search engine + dialog.
- **`src/ui`** — everything else: `MainWindow`, dual file panels, the directory tree, all dialogs (`dialogs/`), theming (`theme/`), i18n (`i18n/`), and the thumbnail pipeline.
- **`src/smbhelper`** — a separate, deliberately Qt-free and FileCommander-free executable (`FileCommander-smb-helper`) that talks only to libsmbclient. It exists because libsmbclient cannot be driven concurrently in-process; `SmbHelperClient`/`SmbHelperProtocol` (in `core/network`) spawn a pool of these as subprocesses so SMB reads can proceed in parallel and a libsmbclient crash can't take the main process down. Built to land next to the main binary in the build tree so a dev build doesn't reach for a system-installed copy.

### The `FileProvider` abstraction (`src/core/filesystem/FileProvider.h`)

Every filesystem backend (local, SFTP, FTP, WebDAV, SMB, archive) implements this interface; `FileSystemModel` and file operations code against it rather than hard-coding local-filesystem calls. Notable contract details worth reading before touching a backend:
- `RenameResult` distinguishes `Unsupported` (backend can't express this — silently fall back) from `Failed` (attempted, genuinely failed) — callers must not conflate them.
- `moveTo()` is a server-side fast path for same-backend moves (measured ~191x faster than stream-down/stream-up on SMB); a non-`Ok` return must guarantee the source was left untouched so the caller can always fall back to copy+delete.
- Streaming I/O (`openRead`/`openWrite`/`read`/`write`/`closeHandleStatus`) backs cross-provider transfers with resume support; `closeHandleStatus` exists because streamed uploads (FTP/WebDAV) only learn the real server-side commit result at close time.
- `maxReadChannels()` lets a backend report real concurrency instead of callers assuming it — SMB reports 1 until helper subprocesses are confirmed up.
- `setModifiedTime()` is best-effort and implemented only where verified against a real server (see the WebDAV/SMB progress in recent commits); a `false` return must never fail the calling transfer.

### Why the custom providers exist instead of gvfs (measured, 2026-07)

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

### Portability: what a Windows port would touch

gvfs does not exist on Windows, which settles the question above from a second direction — the custom providers are the portable asset here, not a Linux-specific workaround.

- **SMB gets easier, not harder.** Windows treats `\\server\share` as an ordinary path, so `LocalFileProvider` covers it and `libsmbclient` (which has no Windows port) plus the whole `src/smbhelper` subprocess pool drop out. That pool only exists because libsmbclient cannot be driven concurrently in-process.
- **Portable as-is:** libssh2 (SFTP), libcurl (FTP/WebDAV), libarchive, libmpv, poppler, Qt5.
- **Needs a platform split:** `ConnectionStore` (libsecret → Credential Manager), trash (`gio trash` → `SHFileOperation`), `GvfsMounter` in its entirety (the nearest equivalent is `WNetAddConnection2` mapping a network drive, which reaches the same "real path for external programs" goal).
- **The "path == local filesystem path" bug class gets worse.** Several operations assume a panel's path can be handed straight to `QFile`/`QDir`/`QProcess`, which is wrong for network *and* archive tabs (see below). Drive letters, UNC paths, separators and case-insensitivity all give that assumption new ways to fail on Windows.

### The "path == local filesystem path" trap

A panel's path belongs to its provider. For a network tab it names something on the server; for an archive tab it names an entry inside the archive. Handing either to `QFile`, `QFileInfo::exists()`, `QDir`, `QUrl::fromLocalFile()`, `QProcess` arguments, or a C library that opens by filename either fails, or — worse — silently operates on a same-named *local* file.

Guards written as `!prov->displayName().isEmpty()` catch network tabs but **not archive tabs**: `ArchiveProvider` does not override `displayName()`, so `FileProvider.h`'s default empty string makes every such guard treat an archive tab as local. Prefer a capability query on `FileProvider` over inferring locality from a display string, and remember that a new backend must default to the safe answer.

`ComputerProvider` (the "Computer" view behind the address row's computer button) is the third such backend and the most synthetic: its paths (`computer://server/<uuid>`) stand for *places* — drives, user folders, removable media, saved bookmarks, discovered hosts — not for files, and one row may not name anything on disk at all. Activating a row therefore never navigates to its path; `FilePanel` reports it through `computerEntryActivated` and `MainWindow` dispatches on `ComputerEntry::Kind`. It also drives the three synthetic-listing hooks on `FileProvider` (`isVirtualListing`/`entryTypeLabel`/`entryIconPath`/`entrySortGroup`), which is how the Type column can say "Server" and how the sections keep their order under any user sort.

### Virtual backends and the tab's real path

An archive browse and the computer view both replace the model's provider for a while. Two things that has to get right, and both have bitten already:

- The tab's parked network connection must be **detached, not torn down** (`FileSystemModel::setProvider()` stops a session when the provider changes under it), so stepping back out finds it alive.
- `saveCurrentTabState()` must record a **real** path, never the synthetic root. That state is what the shutdown snapshot persists, and the next launch hands it to the *local* provider — a persisted `computer://` restores a tab nothing can list.

Every call site that replaces the backend wholesale (tab switch, panel swap, disconnect, favourites, history) goes through `leaveVirtualBackend()` / `backOutOfVirtualBackend()` rather than the archive-specific pair, so a fourth synthetic backend is one place to change, not a dozen.

### Thumbnail pipeline (`src/ui`)

A layered system for populating the icon-view grid without blocking the UI: `ThumbnailCache` (disk-backed cache) → `ThumbnailSweep`/`ThumbnailSweepDrive` (progressive, scroll-driven fetch order — grabs the currently visible screen first) → `RemoteThumbnailFetcher` (network-provider thumbnails) → `Mp4RangePlan`/`VideoRangePlan` (byte-range planning so a remote video's keyframe can be fetched without downloading the whole file) → `ExifThumbnail` (embedded JPEG preview to avoid pulling a full-size remote image). Recent history in this area (see git log) has repeatedly been about keyframe/range-selection correctness for *remote* media — read `tests/ui/test_VideoRangePlan.cpp`, `test_Mp4RangePlan.cpp`, and `test_RemoteThumbnail*.cpp` before changing this path.

### Pending/in-flight design docs

`wps兼容方案.md` (repo root) is a verified technical plan — not yet implemented — for previewing WPS Office's private formats (`.wps`/`.et`/`.dps`) by remapping their extensions to the equivalent OOXML type before invoking `office-oxide`. Check `src/viewer/OfficeConverter.cpp` for whether this has landed before assuming the plan is current.
