#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

make_root() {
    local root="$1"
    mkdir -p "$root/usr/bin" "$root/usr/lib" "$root/usr/share/applications" \
        "$root/usr/share/icons/hicolor/scalable/apps"
    printf '#!/bin/sh\nexit 0\n' > "$root/usr/bin/FileCommander"
    chmod +x "$root/usr/bin/FileCommander"
    printf '[Desktop Entry]\nName=FileCommander\n' > "$root/usr/share/applications/FileCommander.desktop"
    printf '<svg/>\n' > "$root/usr/share/icons/hicolor/scalable/apps/FileCommander.svg"
    printf 'library\n' > "$root/usr/lib/libsample.so"
}

assert_fails_with() {
    local expected="$1"
    shift
    local output
    set +e
    output="$("$@" 2>&1)"
    local status=$?
    set -e
    if [[ "$status" -eq 0 ]]; then
        echo "expected failure containing '$expected', command succeeded" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected"* ]]; then
        echo "expected failure containing '$expected'" >&2
        echo "$output" >&2
        exit 1
    fi
}

root_a="$TMP/a"
root_b="$TMP/b"
make_root "$root_a"
make_root "$root_b"

bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$root_a" appimage "$TMP/a.json"
bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$root_b" appimage "$TMP/b.json"
cmp "$TMP/a.json" "$TMP/b.json"
bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$root_a" "$TMP/a.json" x86_64

cp -a "$root_a" "$TMP/unknown"
bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$TMP/unknown" appimage "$TMP/unknown.json"
printf '#!/bin/sh\nexit 0\n' > "$TMP/unknown/usr/bin/undeclared-tool"
chmod +x "$TMP/unknown/usr/bin/undeclared-tool"
assert_fails_with "usr/bin/undeclared-tool" \
    bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$TMP/unknown" "$TMP/unknown.json" x86_64

for residue in usr/lib/leftover.o usr/lib/archive.a usr/lib/symbols.debug usr/bin/ui_tests usr/bin/wmf_probe; do
    dirty="$TMP/dirty-${residue//\//-}"
    cp -a "$root_a" "$dirty"
    mkdir -p "$dirty/$(dirname "$residue")"
    printf '#!/bin/sh\nexit 0\n' > "$dirty/$residue"
    chmod +x "$dirty/$residue"
    bash "$REPO_ROOT/packaging/write-linux-manifest.sh" "$dirty" appimage "$TMP/dirty.json"
    assert_fails_with "$residue" \
        bash "$REPO_ROOT/packaging/verify-linux-package.sh" "$dirty" "$TMP/dirty.json" x86_64
done

echo "linux manifest tests passed"
