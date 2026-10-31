#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then sanitize=-fsanitize=address,undefined; fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g $sanitize -I main \
    main/epo.c tools/tests/test_epo.c -o "$tmp/test_epo"
"$tmp/test_epo" "$@"
