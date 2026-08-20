#!/usr/bin/env bash
set -euo pipefail

# Every check below reads the English output of file/readelf. On a localized
# builder (zh_CN here) readelf translates even the "Machine:" field label, so
# the ELF architecture check found nothing to compare and failed every build.
export LC_ALL=C

if [[ $# -ne 3 ]]; then
    echo "usage: verify-linux-package.sh <root> <manifest-json> <elf-arch>" >&2
    exit 2
fi

ROOT="$(cd "$1" && pwd)"
MANIFEST="$2"
EXPECTED_ARCH="$3"

if [[ ! -f "$MANIFEST" ]]; then
    echo "error: manifest not found: $MANIFEST" >&2
    exit 1
fi

manifest_paths="$(mktemp)"
manifest_hashes="$(mktemp)"
stage_paths="$(mktemp)"
trap 'rm -f "$manifest_paths" "$manifest_hashes" "$stage_paths"' EXIT

sed -n 's/.*"path": "\([^"]*\)".*/\1/p' "$MANIFEST" | sort > "$manifest_paths"
sed -n 's/.*"path": "\([^"]*\)".*"sha256": "\([0-9A-Fa-f]*\)".*/\1\t\U\2/p' \
    "$MANIFEST" | sort > "$manifest_hashes"

(
    cd "$ROOT"
    find . -type f -print | sed 's#^\./##' | sort
) > "$stage_paths"

unknown="$(comm -13 "$manifest_paths" "$stage_paths" | head -1 || true)"
if [[ -n "$unknown" ]]; then
    echo "error: package contains unknown file: $unknown" >&2
    exit 1
fi

missing="$(comm -23 "$manifest_paths" "$stage_paths" | head -1 || true)"
if [[ -n "$missing" ]]; then
    echo "error: manifest declares missing file: $missing" >&2
    exit 1
fi

while IFS=$'\t' read -r rel expected_sha; do
    [[ -n "$rel" ]] || continue
    actual_sha="$(sha256sum "$ROOT/$rel" | awk '{print toupper($1)}')"
    if [[ "$actual_sha" != "$expected_sha" ]]; then
        echo "error: package file hash differs from manifest: $rel" >&2
        exit 1
    fi
done < "$manifest_hashes"

while IFS= read -r rel; do
    case "$rel" in
        *.o|*.obj|*.a|*.debug|*.dbg|*.pdb)
            echo "error: package contains build/debug residue: $rel" >&2
            exit 1
            ;;
        *tests*|*Tests*|*probe*|*Probe*)
            if [[ -x "$ROOT/$rel" ]]; then
                echo "error: package contains test or probe executable: $rel" >&2
                exit 1
            fi
            ;;
    esac

    if command -v readelf >/dev/null 2>&1 && file "$ROOT/$rel" | grep -q 'ELF'; then
        machine="$(readelf -h "$ROOT/$rel" 2>/dev/null | sed -n 's/.*Machine:[[:space:]]*//p' | head -1)"
        case "$EXPECTED_ARCH" in
            x86_64|amd64)
                if [[ "$machine" != *"X86-64"* && "$machine" != *"Advanced Micro Devices X86-64"* ]]; then
                    echo "error: package file has wrong ELF architecture: $rel" >&2
                    exit 1
                fi
                ;;
            aarch64|arm64)
                if [[ "$machine" != *"AArch64"* ]]; then
                    echo "error: package file has wrong ELF architecture: $rel" >&2
                    exit 1
                fi
                ;;
        esac
    fi
done < "$stage_paths"
