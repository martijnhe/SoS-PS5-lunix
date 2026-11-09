#!/usr/bin/env bash
# Builds the Linux release folder: release/SonsOfSparta-PS5-Native-<version>-linux-x86_64
# Usage: tools/package_linux.sh [version] [build-dir]
set -euo pipefail

version="${1:-0.4.1}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$(cd "${2:-$repo/build-linux}" && pwd)"
out="$repo/release/SonsOfSparta-PS5-Native-$version-linux-x86_64"

rm -rf "$out"
mkdir -p "$out/libs" "$out/tools"
install -m 755 "$build/SonsOfSparta-PS5" "$out/"
install -m 755 "$build/core/relinker/relinker" "$out/tools/"
cp "$build/core/libs/libs/"*.prx "$out/libs/"
cp "$repo/LICENSE" "$out/"
if [ -f "$repo/docs/user/LINUX.md" ]; then cp "$repo/docs/user/LINUX.md" "$out/README-LINUX.md"; fi

tar -C "$repo/release" -czf "$out.tar.gz" "$(basename "$out")"
echo "$out"
echo "$out.tar.gz"
