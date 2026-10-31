#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then sanitize=-fsanitize=address,undefined; fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g $sanitize \
    -I main main/l10n.c tools/tests/l10n/test_l10n.c -o "$tmp/l10n"
"$tmp/l10n"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g $sanitize \
    -I tools/tests/l10n/stubs -I main main/config.c main/layouts.c \
    tools/tests/l10n/test_config_language.c -o "$tmp/config"
"$tmp/config"
python3 tools/tests/l10n/check_catalog.py
