#!/bin/sh
# Bundle the current build for someone else to flash:
#   tools/make_release.sh   -> c606-oss-<version>.zip
# Contents: c606_oss.bin, flash_poc.py, docs/FLASHING.md, VERSION.txt
set -e
cd "$(dirname "$0")/.."
[ -f build/c606_oss.bin ] || { echo "build first (tools/build_podman.sh)"; exit 1; }
VER=$(git describe --always --dirty 2>/dev/null || date +%Y%m%d)
OUT=c606-oss-$VER.zip
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp build/c606_oss.bin tools/flash_poc.py docs/FLASHING.md "$TMP"
{
    echo "c606-oss $VER"
    echo "built:  $(date -u -r build/c606_oss.bin +%Y-%m-%dT%H:%MZ)"
    git log -1 --format='commit: %H (%ci)' 2>/dev/null || true
    echo "image:  $(stat -c %s build/c606_oss.bin) bytes, sha256 $(sha256sum build/c606_oss.bin | cut -c1-64)"
} > "$TMP/VERSION.txt"
rm -f "$OUT"
(cd "$TMP" && zip -q "$OLDPWD/$OUT" c606_oss.bin flash_poc.py FLASHING.md VERSION.txt)
echo "$OUT"
unzip -l "$OUT"
