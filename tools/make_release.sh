#!/bin/sh
# Bundle the current build for someone else to flash:
#   tools/make_release.sh   -> c606-oss-<version>.zip
set -e
cd "$(dirname "$0")/.."
[ -f build/c606_oss.bin ] || { echo "build first (tools/build_podman.sh)"; exit 1; }
VER=$(git describe --always --dirty 2>/dev/null || date +%Y%m%d)
OUT=c606-oss-$VER.zip
rm -f "$OUT"
zip -j "$OUT" build/c606_oss.bin tools/flash_poc.py docs/FLASHING.md
echo "$OUT"
