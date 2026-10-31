#!/bin/sh
# Host-only tests for the GNSS reference-time protocol; no ESP-IDF needed.
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then sanitize=-fsanitize=address,undefined; fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g \
    $sanitize -I main main/gps_aid_protocol.c tools/tests/test_gps_aid_protocol.c \
    -o "$tmp/test_gps_aid"
"$tmp/test_gps_aid"
