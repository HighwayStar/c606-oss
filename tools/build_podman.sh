#!/bin/sh
# Build with the official ESP-IDF container (no host toolchain needed).
#   tools/build_podman.sh            -> idf.py build
#   tools/build_podman.sh fullclean  -> any idf.py args
set -e
cd "$(dirname "$0")/.."
IMG=${IDF_IMAGE:-docker.io/espressif/idf:v5.4.2}
exec podman run --rm -it --network host -v "$PWD:/project:Z" -w /project -e IDF_TARGET=esp32s3 \
    "$IMG" idf.py "${@:-build}"
