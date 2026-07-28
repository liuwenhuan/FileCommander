#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pin_file="$repo_root/third_party/office_oxide.version"
url="$(sed -n 's/^repository=//p' "$pin_file")"
commit="$(sed -n 's/^commit=//p' "$pin_file")"
package="$(sed -n 's/^package=//p' "$pin_file")"
output="${1:-$repo_root/build/office-oxide}"
source_dir="$output/source"
if [[ ! -d "$source_dir/.git" ]]; then git clone "$url" "$source_dir"; fi
git -C "$source_dir" fetch origin "$commit" --depth 1
git -C "$source_dir" checkout --detach "$commit"
test "$(git -C "$source_dir" rev-parse HEAD)" = "$commit"
cargo build --manifest-path "$source_dir/Cargo.toml" --release -p "$package"
mkdir -p "$output"
cp "$source_dir/target/release/office-oxide" "$output/office-oxide"
