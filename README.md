# c606-oss — open firmware for the Magene C606 (proof of concept)

The Magene C606 bike computer is an **ESP32-S3** driving a 240x320 ST7789 over
a 16-bit i80 bus, plus an **nRF co-processor** that owns the buttons, power
management and sensor radios and talks to the ESP32 over UART.

This PoC replaces only the ESP32 application. It:

* initialises the LCD with the vendor's exact init sequence and turns on the backlight,
* opens the UART link to the nRF, sends the vendor's power-on handshake,
* decodes button events and shows them (plus every raw frame) on screen.

Everything hardware-specific lives in `main/board.h`; the analysis behind it
is in [`docs/HARDWARE.md`](docs/HARDWARE.md).

## Build

Needs ESP-IDF **5.3 or newer** (`esp_driver_uart`, `esp_lcd` i80 API).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
```

or without installing anything on the host:

```sh
tools/build_podman.sh          # uses docker.io/espressif/idf:v5.4.2
```

Console logs go to the S3's USB-Serial-JTAG (the USB-C port):
`idf.py -p /dev/ttyACM0 monitor`.

## Flash — without losing the vendor firmware

The bootloader, partition table, NVS (contains the phy calibration and the
vendor's device config) and the nRF are untouched. Only the `ota_0` app slot
is overwritten and `otadata` is erased so the bootloader boots `ota_0`.

```sh
# put the device into download mode (GPIO0 low at reset), then:
tools/flash_poc.py -p /dev/ttyACM0            # backs up ota_0 + otadata first (--full-backup for all 16 MB)
tools/flash_poc.py -p /dev/ttyACM0 --dry-run  # just show the partition table
```

Restore the vendor app:

```sh
tools/flash_poc.py -p /dev/ttyACM0 --restore path/to/vendor_ota_0.bin
```

(`vendor_ota_0.bin` = the `ota_0` partition dumped from your device, e.g.
with `esp32_image_parser.py dump_partition`.)

## What to expect on boot (verified on hardware)

1. Red/green/blue bars with "C606 open FW" for ~1 s (LCD + backlight work).
2. Status screen: uptime, frame counter, `nRF ok r4 fw0.2.19` (power-on
   reason, nRF firmware), `bat 100% 4343mV st0`.
3. Pressing a button adds a row `time key evt aux` and lights the key box
   (keys are idx 0/1/2; event 1 = click, 4 = long press repeating while held, 5 = release). The box is red while a key is long-pressed.

Console: `tools/serial_log.py /dev/ttyACM0 20 --reset` (inside the IDF
container, or anywhere with pyserial) prints the boot log and every non-periodic
nRF frame.

If the screen stays dark: check the backlight (GPIO45) first — the bars are
drawn before it is enabled, so a dark-but-flickering panel means the i80 bus
works. If colours are swapped (red <-> blue) build with
`-DLCD_MADCTL=0x08` (BGR bit) in `main/CMakeLists.txt`.

## Known unknowns / risks

* **Long press on key 0** makes the vendor firmware shut down; the nRF may do a
  hard power-off on its own regardless of what the ESP32 does.
* **PSRAM** is disabled in the PoC. The chip reports embedded 2 MB Quad PSRAM,
  so `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_QUAD=y` should be safe to enable.
* Sensor stream (IMU, barometer) decoding in docs/HARDWARE.md is unverified guesswork.

## Layout

```
main/board.h       pins, bus settings, protocol constants (from RE)
main/lcd.c         i80 bus + ST7789 init + RGB565 framebuffer + text
main/backlight.c   LEDC PWM
main/nrf_link.c    UART framing, CRC16, TX helpers, key decoding
main/main.c        PoC screen
main/font.h        generated 12x20 bitmap font (tools/gen_font.py)
tools/flash_poc.py flash/restore helper
docs/HARDWARE.md   reverse-engineering notes with addresses
```
