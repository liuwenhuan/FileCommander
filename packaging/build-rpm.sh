#!/usr/bin/env bash
#
# Builds FileCommander-<version>-1.x86_64.rpm from the existing CMake install
# tree. This intentionally mirrors build-deb.sh's staged-payload approach: the
# CMake install rules remain the single source of the packaged files.
#
# Usage: packaging/build-rpm.sh [output-dir]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$(cd "${1:-$REPO_ROOT/dist}" 2>/dev/null || { mkdir -p "${1:-$REPO_ROOT/dist}"; cd "${1:-$REPO_ROOT/dist}"; } && pwd)"
BUILD_DIR="$REPO_ROOT/build-rpm"
STAGE_DIR="$BUILD_DIR/stage"
RPM_TOPDIR="$BUILD_DIR/rpmbuild"

for tool in cmake rpmbuild rpm tar strip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool is required to build an RPM" >&2
        exit 1
    }
done

VERSION="$(sed -n 's/^project(FileCommander VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
[[ -n "$VERSION" ]] || { echo "error: could not read project version" >&2; exit 1; }
ARCH="$(rpm --eval '%{_target_cpu}')"
[[ "$ARCH" == "x86_64" ]] || {
    echo "error: RPM releases are currently supported only for x86_64 (got $ARCH)" >&2
    exit 1
}
RELEASE=1
PKG_NAME="FileCommander-${VERSION}-${RELEASE}.${ARCH}.rpm"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DTTC_BUILD_TESTS=OFF \
    -DTTC_BUILD_BENCH=OFF
cmake --build "$BUILD_DIR" --target FileCommander FileCommander-smb-helper -j"$(nproc)"

rm -rf "$STAGE_DIR" "$RPM_TOPDIR"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"
strip --strip-unneeded "$STAGE_DIR/usr/bin/FileCommander"
strip --strip-unneeded "$STAGE_DIR/usr/bin/FileCommander-smb-helper"

install -d "$STAGE_DIR/usr/share/doc/filecommander"
install -m 0644 "$REPO_ROOT/LICENSE" "$STAGE_DIR/usr/share/doc/filecommander/LICENSE"

# Keep Office preview optional, exactly as the other package formats do.
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
else
    echo "warning: office-oxide not found; Office preview is not included" >&2
fi

RELEASE_MANIFEST="$BUILD_DIR/FileCommander-${VERSION}-${RELEASE}.${ARCH}.manifest.json"
bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$STAGE_DIR" rpm "$RELEASE_MANIFEST"
bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$STAGE_DIR" "$RELEASE_MANIFEST" "$ARCH"

mkdir -p "$RPM_TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
tar -C "$STAGE_DIR" -czf "$RPM_TOPDIR/SOURCES/filecommander-stage.tar.gz" .
cat > "$RPM_TOPDIR/SPECS/filecommander.spec" <<EOF
Name:           filecommander
Version:        $VERSION
Release:        $RELEASE%{?dist}
Summary:        Dual-pane file manager
License:        GPL-3.0-or-later
URL:            https://github.com/liuwenhuan/FileCommander
Source0:        filecommander-stage.tar.gz
BuildArch:      x86_64
Requires:       policycoreutils
Recommends:     p7zip p7zip-plugins squashfs-tools ffmpeg libjpeg-turbo-utils udisks2

%description
A dual-pane file manager for X11 desktops. It browses local directories,
archives, and SFTP, SMB, FTP, and WebDAV shares with built-in file previews.

%prep
%setup -q -c -T

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}

%post
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q /usr/share/applications || :
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || :
fi

%postun
if [ \$1 -eq 0 ]; then
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || :
  fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || :
  fi
fi

%files
/usr/bin/FileCommander
/usr/bin/FileCommander-smb-helper
/usr/share/applications/*
/usr/share/icons/hicolor/*
/usr/share/metainfo/*
/usr/share/doc/filecommander/LICENSE
EOF

# Add an optional Office sidecar to the file list only when it was staged.
if [[ -x "$STAGE_DIR/usr/bin/office-oxide" ]]; then
    printf '%s\n' '/usr/bin/office-oxide' >> "$RPM_TOPDIR/SPECS/filecommander.spec"
fi

rpmbuild -bb "$RPM_TOPDIR/SPECS/filecommander.spec" --define "_topdir $RPM_TOPDIR"
RPM_FILE="$(find "$RPM_TOPDIR/RPMS/$ARCH" -maxdepth 1 -name 'filecommander-*.rpm' -print -quit)"
[[ -n "$RPM_FILE" ]] || { echo "error: rpmbuild did not create an RPM" >&2; exit 1; }

mkdir -p "$OUT_DIR"
install -m 0644 "$RPM_FILE" "$OUT_DIR/$PKG_NAME"
install -m 0644 "$RELEASE_MANIFEST" "$OUT_DIR/$PKG_NAME.manifest.json"
rpm -K "$OUT_DIR/$PKG_NAME"
rpm -qlp "$OUT_DIR/$PKG_NAME" | grep -qx '/usr/bin/FileCommander'
rpm -qlp "$OUT_DIR/$PKG_NAME" | grep -qx '/usr/bin/FileCommander-smb-helper'
rpm -qip "$OUT_DIR/$PKG_NAME" | grep -q "Architecture.*$ARCH"

echo "==> $OUT_DIR/$PKG_NAME"
sha256sum "$OUT_DIR/$PKG_NAME"
