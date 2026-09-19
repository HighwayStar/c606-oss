#!/bin/sh
# Bundle the current build for someone else to flash:
#   tools/make_release.sh              -> c606-oss-<version>.zip
#   BOARD=c706 tools/make_release.sh   -> c706-oss-<version>.zip (from build-c706/)
# Contents: <board>_oss.bin, flash_poc.py, docs/FLASHING.md, VERSION.txt
set -e
cd "$(dirname "$0")/.."
BOARD=${BOARD:-c606}
if [ "$BOARD" = c606 ]; then BDIR=build; else BDIR=build-$BOARD; fi
BIN=$BDIR/${BOARD}_oss.bin
[ -f "$BIN" ] || { echo "build first (BOARD=$BOARD tools/build_podman.sh)"; exit 1; }
VER=$(git describe --always --dirty 2>/dev/null || date +%Y%m%d)
OUT=$BOARD-oss-$VER.zip
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$BIN" tools/flash_poc.py docs/FLASHING.md "$TMP"
{
    echo "c606-oss $VER for the Magene $(echo "$BOARD" | tr a-z A-Z)"
    echo "built:  $(date -u -r "$BIN" +%Y-%m-%dT%H:%MZ)"
    git log -1 --format='commit: %H (%ci)' 2>/dev/null || true
    echo "image:  $(stat -c %s "$BIN") bytes, sha256 $(sha256sum "$BIN" | cut -c1-64)"
} > "$TMP/VERSION.txt"
rm -f "$OUT"
(cd "$TMP" && zip -q "$OLDPWD/$OUT" "${BOARD}_oss.bin" flash_poc.py FLASHING.md VERSION.txt)
echo "$OUT"
unzip -l "$OUT"
