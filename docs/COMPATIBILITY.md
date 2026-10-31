# Multi-board compatibility review

Review baseline: upstream `30c7aee` through custom changes `bbedc79`.
Toolchain: ESP-IDF 5.4.2, ESP32-S3. Each target uses an independent build
directory and a fresh sdkconfig, combining the shared defaults with its
board-specific defaults.

| Target | Display | Flash / PSRAM mode | Keys / initial GPS baud |
|---|---|---|---|
| C606 | 240 x 320, ST7789, 16-bit bus | 16 MB / quad | 3 / 921600 |
| C606 Pro | 240 x 320, ST7789 Pro init, 8-bit bus | 16 MB / octal | 3 / 921600 |
| CC700 Pro | 240 x 320, ST7789 C606 init, 8-bit bus | 16 MB / octal | 3 / 921600 |
| C706 | 320 x 480, AXS15231, 8-bit bus | 32 MB / octal | 5 / 115200 |

## Findings and fixes

- A saved sdkconfig can override the board choice supplied by defaults.
  CMake now rejects a mismatch between `BOARD` and the effective Kconfig
  board before generating an image with a misleading model name. Unknown
  board names get an explicit diagnostic.
- The flash helper previously selected the first model whose image existed.
  With builds for several models present this could select a C606 image for
  another device. It now requires `--board` or `--app` in that case, before
  opening the serial port. A single-model release remains automatic; explicit
  images and vendor restoration retain their existing behavior.
- Localized toggle labels used the full row width, including the switch.
  They now reserve the switch's 44 px plus an 8 px gap and truncate within
  the remaining space on both screen sizes.

## Shared features

- Map header placement and screenshot buffers use `LCD_H_RES` / `LCD_V_RES`.
  The C706's four-pixel display alignment remains handled by `ui_port.c`.
  Auto zoom uses the same speed policy on all targets; no C606 pin or panel
  constants were introduced in it.
- Key handling and translated key hints use the board's `KEY_*` definitions.
  Wi-Fi credential entry uses the touchscreen on every model.
- The config blob and field IDs are independent of the board. The v14/v15
  migration tests verify preservation of existing settings when adding
  language and auto zoom (v16, 196 bytes).
- Wi-Fi and USB use the ESP32-S3 drivers common to all four targets. TLS
  allocations use PSRAM and certificate validation stays enabled. When
  updating an existing checkout, defaults do not override a saved sdkconfig:
  check `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` there as well.
- GPS uses each board's initial baud and the existing baud probe. PAIR590
  and PAIR471 wait for receiver acknowledgements and report failure on
  rejection/timeout. End-to-end Wi-Fi / Host EPO acceptance is verified only
  on the tested C606. A shared Airoha protocol and successful compilation do
  not establish receiver-firmware support or faster fixes on the other units.
- C706 vendor `.etu` maps are not supported. Copy a Mapsforge `.map` file
  for the map page and auto zoom, as described in HARDWARE.md.
- `QUICK_START_A4.pdf` describes C606 controls. Use the model-specific key
  table in USER_GUIDE.md for C706's five buttons.

## Verification and limits

All four builds passed this review, including the fixes above. Inspection
of the effective sdkconfigs and ESP image headers confirmed the expected
board, PSRAM mode, flash size, TLS-in-PSRAM, certificate bundle, USB console
and embedded project name. Image sizes: C606 1,766,384 bytes; C606 Pro
1,769,984; CC700 Pro 1,770,144; C706 1,770,608. All fit their app slots.
The six host test commands below passed. Negative configure checks also
passed: unknown BOARD and C706 with a copied C606 sdkconfig were rejected.

Build all four targets with ESP-IDF 5.4.2, then inspect each generated
sdkconfig for its board, flash size, PSRAM mode and TLS allocation mode.
Keep a separate sdkconfig per target, for example:

```sh
idf.py -B build-c706 -DBOARD=c706 build
python3 tools/tests/test_flash_selection.py
sh tools/test_l10n.sh
sh tools/test_epo.sh
sh tools/test_gps_aid.sh
sh tools/test_map_header.sh
sh tools/test_map_zoom.sh
```

The flash-selection regression test reproduces the old ambiguity and checks
all four explicit selections, single-model releases and failure before any
serial access. Also check that configuring a different `BOARD` against a
copied sdkconfig fails with the board-mismatch diagnostic.

Hardware status in README.md describes the upstream ports: C606, C706 and
CC700 Pro have been run on hardware; C606 Pro remains untested on a unit,
and ANT+ on CC700 Pro is not yet confirmed. This review does not extend
those hardware claims to our new features on devices we do not have.
