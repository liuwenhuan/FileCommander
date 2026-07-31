#!/usr/bin/env bash
#
# Builds FileCommander_<version>_<arch>.deb.
#
# Deliberately uses dpkg-deb over a staging tree rather than a full
# debhelper/dpkg-buildpackage source package: this repo has no Debian source
# package to maintain, and the CMake install rules already describe the whole
# payload. See docs/PACKAGING.md.
#
# Usage: packaging/build-deb.sh [output-dir]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$(cd "${1:-$REPO_ROOT/dist}" 2>/dev/null || { mkdir -p "${1:-$REPO_ROOT/dist}"; cd "${1:-$REPO_ROOT/dist}"; } && pwd)"
BUILD_DIR="$REPO_ROOT/build-deb"
STAGE_DIR="$BUILD_DIR/stage"

# Single source of truth for the version: the project() line. Keeping this
# derived rather than hardcoded stops the package version from drifting away
# from the TTC_VERSION compiled into the binary (used by the update checker).
VERSION="$(sed -n 's/^project(FileCommander VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
if [[ -z "$VERSION" ]]; then
    echo "error: could not read version from CMakeLists.txt project() line" >&2
    exit 1
fi
ARCH="$(dpkg --print-architecture)"
PKG_NAME="FileCommander_${VERSION}_${ARCH}.deb"

echo "==> Building FileCommander $VERSION ($ARCH)"

# --- Build ------------------------------------------------------------------
# Release matters: the project's CMakeLists defaults to Debug when no build type
# is given, which produces a ~36 MB unoptimised binary instead of ~3 MB.
# Tests/bench are off: they triple build time and pull in a googletest checkout.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DTTC_BUILD_TESTS=OFF \
    -DTTC_BUILD_BENCH=OFF

# FileCommander-smb-helper is a separate executable, so it needs naming here: without it
# SMB thumbnails silently fall back to the slower single-channel path.
cmake --build "$BUILD_DIR" --target FileCommander FileCommander-smb-helper -j"$(nproc)"

# --- Stage ------------------------------------------------------------------
rm -rf "$STAGE_DIR"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"

strip --strip-unneeded "$STAGE_DIR/usr/bin/FileCommander"
strip --strip-unneeded "$STAGE_DIR/usr/bin/FileCommander-smb-helper"

install -d "$STAGE_DIR/usr/share/doc/filecommander"
gzip -9cn "$REPO_ROOT/docs/UPDATE_SERVER.md" > "$STAGE_DIR/usr/share/doc/filecommander/UPDATE_SERVER.md.gz"

# office-oxide renders Office documents for the preview pane. It is a separate
# project with no distro package, so it ships here rather than as a dependency;
# without it, Office preview silently shows nothing. Found the same way the app
# looks for it at runtime (see OfficeConverter::resolveBinary), so whatever the
# build host uses is what gets packaged.
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
    install -m 0755 "$(readlink -f "$OXIDE")" "$STAGE_DIR/usr/bin/office-oxide"
    strip --strip-unneeded "$STAGE_DIR/usr/bin/office-oxide" 2>/dev/null || true
    echo "==> Bundled office-oxide from $OXIDE"
else
    echo "warning: office-oxide not found; Office document preview will not work" >&2
fi

# --- Dependencies -----------------------------------------------------------
# Prefer dpkg-shlibdeps: it reads the actual ELF and resolves each SONAME to the
# package that ships it, so the list can't drift as the link line changes. The
# static fallback below is only for builders without dpkg-dev, and was derived
# by running dpkg -S over this binary's NEEDED entries.
SHLIB_DEPS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "==> Resolving dependencies with dpkg-shlibdeps"
    # dpkg-shlibdeps insists on running from a tree with debian/control.
    SHLIBS_TMP="$BUILD_DIR/shlibdeps"
    rm -rf "$SHLIBS_TMP" && mkdir -p "$SHLIBS_TMP/debian"
    touch "$SHLIBS_TMP/debian/control"
    if (cd "$SHLIBS_TMP" && dpkg-shlibdeps -O --ignore-missing-info \
            "$STAGE_DIR/usr/bin/FileCommander" 2>/dev/null) > "$SHLIBS_TMP/out"; then
        SHLIB_DEPS="$(sed -n 's/^shlibs:Depends=//p' "$SHLIBS_TMP/out")"
    fi
fi

if [[ -z "$SHLIB_DEPS" ]]; then
    echo "==> dpkg-shlibdeps unavailable or failed; using the static dependency list"
    SHLIB_DEPS="libqt5core5a, libqt5gui5, libqt5widgets5, libqt5concurrent5, \
libqt5dbus5, libqt5network5, libqt5opengl5, libqt5x11extras5, libarchive13, \
libssh2-1, libsecret-1-0, libcurl4, libsmbclient0, libmpv2, libpoppler-qt5-1, \
libxcb1, zlib1g, libglib2.0-0, libc6, libstdc++6"
fi

# pkexec is not a library dependency, so shlibdeps can't see it: the deb-flavour
# self-update path shells out to `pkexec apt-get install` (src/core/update/Updater.cpp).
DEPENDS="$SHLIB_DEPS, policykit-1"

# --- Control ----------------------------------------------------------------
INSTALLED_SIZE="$(du -sk "$STAGE_DIR" | cut -f1)"
install -d "$STAGE_DIR/DEBIAN"
cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: filecommander
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: p7zip-full, squashfs-tools, ffmpeg, libjpeg-turbo-progs, udisks2
Suggests: unrar, avahi-daemon, gvfs-backends
Installed-Size: $INSTALLED_SIZE
Maintainer: FileCommander developers <filecommander@localhost>
Description: FileCommander - dual-pane file manager
 A dual-pane file manager for X11 desktops. Browses local directories,
 archives, and remote shares (SFTP, SMB, FTP, WebDAV) in the same view,
 with built-in preview for text, images, video, PDF, and office documents.
 .
 p7zip-full is recommended rather than p7zip: only the full 7z binary can
 read UDF disc images, which is what most modern install ISOs are.
EOF

# Update the icon cache and desktop database so the launcher entry appears
# without a re-login. Both are best-effort: a headless install has neither.
cat > "$STAGE_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
EOF

cat > "$STAGE_DIR/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
EOF

chmod 0755 "$STAGE_DIR/DEBIAN/postinst" "$STAGE_DIR/DEBIAN/postrm"

RELEASE_MANIFEST="$BUILD_DIR/$PKG_NAME.manifest.json"
bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$STAGE_DIR" deb "$RELEASE_MANIFEST"
bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$STAGE_DIR" "$RELEASE_MANIFEST" "$ARCH"

# --- Package ----------------------------------------------------------------
mkdir -p "$OUT_DIR"
dpkg-deb --build --root-owner-group "$STAGE_DIR" "$OUT_DIR/$PKG_NAME"
cp "$RELEASE_MANIFEST" "$OUT_DIR/$PKG_NAME.manifest.json"

VERIFY_EXTRACT="$BUILD_DIR/deb-extract-verify"
rm -rf "$VERIFY_EXTRACT"
mkdir -p "$VERIFY_EXTRACT"
dpkg-deb -R "$OUT_DIR/$PKG_NAME" "$VERIFY_EXTRACT"
bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$VERIFY_EXTRACT" \
    "$OUT_DIR/$PKG_NAME.manifest.json" "$ARCH"

echo
echo "==> $OUT_DIR/$PKG_NAME"
sha256sum "$OUT_DIR/$PKG_NAME"
