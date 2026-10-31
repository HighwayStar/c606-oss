#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I main \
    main/map_header.c tools/tests/test_map_header.c -lm -o "$tmp/test"
"$tmp/test"
