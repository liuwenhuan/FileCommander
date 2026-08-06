#!/usr/bin/env bash
#
# Builds FileCommander-<version>-x86_64.AppImage.
#
# The resulting AppImage must be launched through a real type-2 runtime, because
# $APPIMAGE is what UpdateChecker::runningAsAppImage() reads to pick the
# manifest segment this build should be told about.
# A bare self-extracting archive would leave that unset and send the updater
# down the .deb path. appimagetool (invoked by linuxdeploy) gives us that.
#
# Usage: packaging/build-appimage.sh [output-dir]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ARG="${1:-$REPO_ROOT/dist}"
mkdir -p "$OUT_ARG"
OUT_DIR="$(cd "$OUT_ARG" && pwd)"
BUILD_DIR="$REPO_ROOT/build-appimage"
APPDIR="$BUILD_DIR/AppDir"
TOOLS_DIR="$REPO_ROOT/packaging/.tools"

VERSION="$(sed -n 's/^project(FileCommander VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
if [[ -z "$VERSION" ]]; then
    echo "error: could not read version from CMakeLists.txt project() line" >&2
    exit 1
fi

echo "==> Building FileCommander $VERSION AppImage"

# --- Toolchain --------------------------------------------------------------
fetch_tool() {
    local name="$1" url="$2" dest="$TOOLS_DIR/$1"
    if [[ ! -x "$dest" ]]; then
        echo "==> Fetching $name"
        mkdir -p "$TOOLS_DIR"
        curl -fsSL -o "$dest" "$url"
        chmod +x "$dest"
    fi
}

fetch_tool linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch_tool linuxdeploy-plugin-qt-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"

# Without a usable FUSE setup the tools can't mount themselves; they can still
# unpack and run in place.
#
# Both halves are checked, because they fail independently. A container or CI
# runner typically has no /dev/fuse at all. WSL has the device but no
# fusermount binary, and the AppImage runtime then dies with "No suitable
# fusermount binary found on the $PATH" -- from inside linuxdeploy, several
# steps into the build, which reads as a linuxdeploy bug rather than a missing
# dependency.
#
# Adjacent trap, written down because it cost an hour and pointed at the wrong
# culprit: a tree copied from a WINDOWS checkout carries CRLF in every text file
# git materialised -- not only the .sh scripts, but resources/icons/
# FileCommander.svg and the .desktop entry. linuxdeploy then rejects the icon
# with "Could not find suitable icon for Icon entry: FileCommander", which reads
# as a missing file and is not; pinning linuxdeploy to an older release changes
# nothing, because upstream is not at fault. Strip CR from the .sh, .svg,
# .desktop and .xml inputs before building from such a tree. A normal Linux
# checkout is unaffected.
if [[ ! -w /dev/fuse ]] || ! command -v fusermount >/dev/null 2>&1; then
    echo "==> no usable FUSE (device or fusermount); using --appimage-extract-and-run"
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

# --- Build ------------------------------------------------------------------
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DTTC_BUILD_TESTS=OFF \
    -DTTC_BUILD_BENCH=OFF

# FileCommander-smb-helper is a separate executable, so it needs naming here: without it
# SMB thumbnails silently fall back to the slower single-channel path.
cmake --build "$BUILD_DIR" --target FileCommander FileCommander-smb-helper -j"$(nproc)"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

# GPL-3 section 4: whoever receives the program receives the terms with it. An
# AppImage is a single file a user may well have got without ever seeing this
# repository, so the licence has to be inside it.
install -d "$APPDIR/usr/share/doc/filecommander"
install -m 0644 "$REPO_ROOT/LICENSE" "$APPDIR/usr/share/doc/filecommander/LICENSE"
strip --strip-unneeded "$APPDIR/usr/bin/FileCommander"
strip --strip-unneeded "$APPDIR/usr/bin/FileCommander-smb-helper"

# --- Bundle the external tools FileCommander shells out to -----------------------------
# 7z is what makes UDF disc images browsable (see ExternalArchiveTool), and
# unsquashfs is what makes AppImages browsable. Bundling them keeps those
# features working on a host that has neither.
#
# Note /usr/bin/7z is only a shell wrapper around /usr/lib/p7zip/7z, which
# dlopen()s 7z.so from the same directory. Both files must travel together, so
# we reproduce that layout and supply our own wrapper.
#
# Only the full `7z` reads UDF -- 7za/7zr report zero entries on those images --
# so we deliberately bundle 7z and not the smaller variants.
install -d "$APPDIR/usr/bin" "$APPDIR/usr/lib/p7zip"
if [[ -f /usr/lib/p7zip/7z && -f /usr/lib/p7zip/7z.so ]]; then
    cp /usr/lib/p7zip/7z /usr/lib/p7zip/7z.so "$APPDIR/usr/lib/p7zip/"
    cat > "$APPDIR/usr/bin/7z" <<'EOF'
#!/bin/sh
# Resolve the real 7z next to this wrapper, mirroring the p7zip layout so the
# binary can dlopen 7z.so from its own directory.
here="$(dirname "$(readlink -f "$0")")"
exec "$here/../lib/p7zip/7z" "$@"
EOF
    chmod +x "$APPDIR/usr/bin/7z"
else
    echo "warning: /usr/lib/p7zip/7z not found; UDF/RAR support will fall back to the host" >&2
fi

if command -v unsquashfs >/dev/null 2>&1; then
    cp "$(command -v unsquashfs)" "$APPDIR/usr/bin/unsquashfs"
else
    echo "warning: unsquashfs not found; AppImage browsing will fall back to the host" >&2
fi

# office-oxide renders Office documents for the preview pane. Searched exactly
# as the app searches at runtime (OfficeConverter::resolveBinary), including the
# cargo/local install dirs it is usually built into.
OXIDE=""
for candidate in office_oxide office-oxide oxide; do
    OXIDE="$(command -v "$candidate" 2>/dev/null || true)"
    [[ -n "$OXIDE" ]] && break
    for dir in "$HOME/.local/bin" "$HOME/.cargo/bin"; do
        if [[ -x "$dir/$candidate" ]]; then
            OXIDE="$dir/$candidate"
            break 2
        fi
    done
done

if [[ -n "$OXIDE" ]]; then
    cp "$(readlink -f "$OXIDE")" "$APPDIR/usr/bin/office-oxide"
    chmod +x "$APPDIR/usr/bin/office-oxide"
    echo "==> Bundled office-oxide from $OXIDE"
else
    echo "warning: office-oxide not found; Office document preview will not work" >&2
fi

# --- Runtime hook -----------------------------------------------------------
# AppRun sources every apprun-hooks/*.sh before exec'ing the app. Ours drops
# host-only Qt plugin names (Deepin's dxcb would otherwise abort startup) and
# puts the bundled tools on PATH. Must be installed before linuxdeploy runs, so
# it ends up inside the image.
install -d "$APPDIR/apprun-hooks"
install -m 0644 "$REPO_ROOT/packaging/apprun-hook.sh" "$APPDIR/apprun-hooks/FileCommander-hook.sh"

# --- Package ----------------------------------------------------------------
# linuxdeploy-plugin-qt supplies the platform plugin (libqxcb -- the app forces
# QT_QPA_PLATFORM=xcb) and the image format plugins, including libqsvg, which
# the 17 SVG icons in resources.qrc need in order to render at all.
export QMAKE="${QMAKE:-/usr/lib/qt5/bin/qmake}"
[[ -x "$QMAKE" ]] || QMAKE="$(command -v qmake)"
export VERSION

cd "$BUILD_DIR"
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/FileCommander.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/FileCommander.svg" \
    --output appimage

# linuxdeploy names the output from the desktop entry + $VERSION; normalise it
# to the name the release checklist commits to.
PRODUCED="$(find "$BUILD_DIR" -maxdepth 1 -name '*.AppImage' -newer "$APPDIR/usr/bin/FileCommander" | head -1)"
if [[ -z "$PRODUCED" ]]; then
    echo "error: linuxdeploy did not produce an AppImage" >&2
    exit 1
fi

APPDIR_MANIFEST="$BUILD_DIR/FileCommander-${VERSION}-x86_64.AppDir.manifest.json"
bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$APPDIR" appimage "$APPDIR_MANIFEST"
bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$APPDIR" "$APPDIR_MANIFEST" x86_64

FINAL="$OUT_DIR/FileCommander-${VERSION}-x86_64.AppImage"
mv "$PRODUCED" "$FINAL"
chmod +x "$FINAL"
cp "$APPDIR_MANIFEST" "$FINAL.manifest.json"

EXTRACT_DIR="$BUILD_DIR/appimage-extract-verify"
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
if (cd "$EXTRACT_DIR" && "$FINAL" --appimage-extract >/dev/null 2>&1); then
    bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$EXTRACT_DIR/squashfs-root" \
        "$FINAL.manifest.json" x86_64
else
    echo "warning: could not extract AppImage for second-pass manifest verification" >&2
fi

echo
echo "==> $FINAL"
sha256sum "$FINAL"
