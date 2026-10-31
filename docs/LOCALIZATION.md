# Languages and Cyrillic fonts

Settings -> Language selects **English** or **Русский**. The choice is saved
immediately and the settings menu is rebuilt. Data-page names, main-page
controls and live values use the selected language without restarting a ride.
New installations and existing configurations default to English. Resetting
settings also returns to English.

## Translation code

`main/l10n_ru.inc` is the UTF-8 Russian catalog. English strings are lookup
keys; do not translate storage paths, protocol commands or log diagnostics.
`tr()` returns constant strings and is safe across tasks. Unknown keys pass
through unchanged. Use `l10n_format()` for translated printf templates: it
returns the actual number of bytes written and does not cut a UTF-8 character
at the buffer end. Preserve format argument types and order in the catalog.
Leave filenames and network credentials as user data.

The menu, ride controls and summaries, data fields and units, map layers,
sensor UI, profile, USB mode and Wi-Fi messages are translated. Diagnostic
logs and developer-console commands stay in English for compatibility with
upstream troubleshooting tools. Proper names (ANT+, GPS, FTP, SRAM, etc.)
are not translated.

The language byte is appended in config version 15, occupying byte 191
(previous tail padding); the blob remains 192 bytes. Migration explicitly
initializes it to English for older blobs, regardless of old padding bytes.
Existing field IDs, routes, sensor pairing, profile and other settings remain
unchanged. See `tools/tests/l10n/test_config_language.c`.

## Fonts

`main/ui_fonts.c` copies the original LVGL font descriptors and attaches
DejaVu Sans Cyrillic fallbacks. Original Latin glyphs, digits and icons are
retained. Generated `font_cyrillic_*.c` files cover Cyrillic (including Ё/ё)
and typographic punctuation at 8, 14, 20, 28 and 48 pixels. Their source font
license is in `main/DejaVu-LICENSE.txt`.

Regenerate with Node/npm and DejaVu Sans installed:

```sh
sh tools/generate_cyrillic.sh /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
```

The converter is pinned to `lv_font_conv@1.5.3`, at 4 bits per pixel without
compression. Binary font artifacts are compiled into the app; no additional
files need to be installed on the bike computer's eMMC.

## Tests

```sh
sh tools/test_l10n.sh
SANITIZE=1 sh tools/test_l10n.sh
idf.py -DBOARD=c606 build
```

Host tests cover catalog lookups, invalid language selection, UTF-8 truncation,
printf signatures, duplicate keys, Cyrillic glyph coverage in every font,
config-v14 migration and language persistence. On hardware, verify Settings,
Language, Profile, data-page chooser, a ride summary and USB mode in both
languages, then restart and check the saved language. Recheck a dense data
layout: translated field names may be ellipsized to fit the same cells.
