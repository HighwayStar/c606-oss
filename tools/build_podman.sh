#!/bin/sh
# Build with the official ESP-IDF container (no host toolchain needed).
#   tools/build_podman.sh                 -> idf.py build          (C606, build/)
#   BOARD=c706 tools/build_podman.sh      -> C706 build in build-c706/ (also c606pro, cc700pro)
#   tools/build_podman.sh fullclean       -> any idf.py args
set -e
cd "$(dirname "$0")/.."
IMG=${IDF_IMAGE:-docker.io/espressif/idf:v5.4.2}
BOARD=${BOARD:-c606}
[ -t 0 ] && TTY=-it
if [ "$BOARD" = c606 ]; then BDIR=build; else BDIR=build-$BOARD; fi
exec podman run --rm $TTY --network host -v "$PWD:/project:Z" -w /project -e IDF_TARGET=esp32s3 \
    "$IMG" idf.py -B "$BDIR" -DBOARD="$BOARD" "${@:-build}"
