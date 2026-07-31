#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: write-linux-manifest.sh <root> <profile> <output-json>" >&2
    exit 2
fi

ROOT="$(cd "$1" && pwd)"
PROFILE="$2"
OUTPUT="$3"
ARCH="$(uname -m)"

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

provenance_for() {
    local path="$1"
    case "$path" in
        usr/bin/FileCommander) echo "application" ;;
        usr/bin/FileCommander-smb-helper) echo "network" ;;
        usr/bin/office-oxide) echo "office" ;;
        usr/bin/7z|usr/bin/unsquashfs|usr/lib/p7zip/*) echo "tools" ;;
        usr/share/applications/*) echo "desktop" ;;
        usr/share/icons/*) echo "icons" ;;
        usr/share/metainfo/*) echo "metadata" ;;
        DEBIAN/*) echo "debian-control" ;;
        *.so|*.so.*|usr/lib/*) echo "libraries" ;;
        *) echo "data" ;;
    esac
}

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

(
    cd "$ROOT"
    find . -type f -print0 | sort -z | while IFS= read -r -d '' file; do
        rel="${file#./}"
        bytes="$(stat -c '%s' "$rel")"
        sha="$(sha256sum "$rel" | awk '{print toupper($1)}')"
        prov="$(provenance_for "$rel")"
        printf '%s\t%s\t%s\t%s\n' "$rel" "$bytes" "$sha" "$prov"
    done
) > "$tmp"

mkdir -p "$(dirname "$OUTPUT")"
{
    printf '{\n'
    printf '  "profile": "%s",\n' "$(json_escape "$PROFILE")"
    printf '  "architecture": "%s",\n' "$(json_escape "$ARCH")"
    printf '  "files": [\n'
    first=1
    while IFS=$'\t' read -r rel bytes sha prov; do
        if [[ "$first" -eq 0 ]]; then
            printf ',\n'
        fi
        first=0
        printf '    { "path": "%s", "bytes": %s, "sha256": "%s", "provenance": "%s", "version": "" }' \
            "$(json_escape "$rel")" "$bytes" "$sha" "$(json_escape "$prov")"
    done < "$tmp"
    printf '\n'
    printf '  ]\n'
    printf '}\n'
} > "$OUTPUT"
