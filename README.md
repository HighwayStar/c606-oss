# c606-oss — open firmware for the Magene C606 (proof of concept)

The Magene C606 bike computer is an **ESP32-S3** driving a 240x320 ST7789 over
a 16-bit i80 bus, plus an **nRF co-processor** that owns the buttons, power
management and sensor radios and talks to the ESP32 over UART.

This PoC replaces only the ESP32 application. It:

* initialises the LCD with the vendor's exact init sequence and turns on the backlight,
* runs **LVGL 9** (draw buffers in internal DMA RAM, objects in the 2 MB PSRAM),
* opens the UART link to the nRF, sends the vendor's power-on handshake,
* decodes button, battery, temperature/pressure and version frames,
* reads the Airoha AG3352Q GNSS on UART0 (auto-baud, NMEA RMC/GGA/GSV),
* keys: 0 = switch page, 1/2 = backlight down/up.

Everything hardware-specific lives in `main/board.h`; the analysis behind it
is in [`docs/HARDWARE.md`](docs/HARDWARE.md).

## Build

Needs ESP-IDF **5.3 or newer** (`esp_driver_uart`, `esp_lcd` i80 API) and
network access on first build (LVGL comes from the component registry, see
`main/idf_component.yml`).

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

1. Status page: header with battery %, battery arc with mV, temperature and
   pressure (once the nRF starts its sensor stream), `nRF ok reason 4 fw 0.2.19`,
   three key boxes, an event log and a footer with free internal/PSRAM heap.
   The `GPS` line shows fix state, satellites used/in view and HDOP.
2. Key 0 click switches to the "ride" page: UTC time, big speed digits,
   position/altitude/course once there is a fix, temperature.
   Key 1 / key 2 step the backlight by 10 %. A key box flashes green on a
   click and stays red while long-pressed (event 4), clears on release (5).

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
* Sensor stream (IMU, barometer) decoding in docs/HARDWARE.md is unverified guesswork.

## Layout

```
main/board.h       pins, bus settings, protocol constants (from RE)
main/lcd.c         i80 bus + ST7789 init + async bitmap push
main/ui_port.c     LVGL 9 display driver, tick, render task, lock
main/ui.c          demo pages (status / ride)
main/backlight.c   LEDC PWM
main/nrf_link.c    UART framing, CRC16, TX helpers, key decoding
main/gps.c         UART0 NMEA reader with baud probing
main/main.c        glue: frame decoding -> UI, key actions, handshake
tools/flash_poc.py flash/restore helper
docs/HARDWARE.md   reverse-engineering notes with addresses
```
