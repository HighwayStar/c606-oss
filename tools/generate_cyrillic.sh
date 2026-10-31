#!/bin/sh
# Requires Node/npm and a DejaVu Sans TTF (fonts-dejavu-core on Debian/Ubuntu).
set -eu
cd "$(dirname "$0")/.."
font=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
for size in 8 14 20 28 48; do
    npx --yes --package lv_font_conv@1.5.3 lv_font_conv \
        --font "$font" --size "$size" --bpp 4 \
        --range '0x400-0x45F,0x2010-0x2026,0x2116' \
        --format lvgl --lv-include lvgl.h --no-compress \
        --lv-font-name "font_cyrillic_$size" -o "main/font_cyrillic_$size.c"
    # The converter appends an empty line after the final #endif.
    sed -i '${/^$/d;}' "main/font_cyrillic_$size.c"
done
