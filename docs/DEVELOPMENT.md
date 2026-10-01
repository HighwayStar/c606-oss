# Development

Building, flashing from the source tree, the debug tools and the code
layout. Hardware and reverse-engineering notes are in
[HARDWARE.md](HARDWARE.md).

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

## Targets

| `BOARD` | device | build dir / image | differences |
|---|---|---|---|
| `c606` (default) | Magene C606 | `build/c606_oss.bin` | ST7789 240x320, 16-bit i80, 16 MB flash, 2 MB quad PSRAM, 3 keys |
| `c606pro` | Magene C606 Pro | `build-c606pro/c606pro_oss.bin` | as the C606 but ST7789 on the C706's 8-bit i80 pins (byte-swapped pixels, its own init sequence), octal PSRAM — untested on hardware (it does run on a CC700 Pro) |
| `cc700pro` | Geoid CC700 Pro | `build-cc700pro/cc700pro_oss.bin` | the C606 Pro's pins, 8-bit bus and 8 MB octal PSRAM with the C606's ST7789 init sequence and GPIO43/44 driven high — verified on hardware (see below) |
| `c706` | Magene C706 | `build-c706/c706_oss.bin` | AXS15231 320x480, 8-bit i80 (pixels byte-swapped, 4-px aligned windows), panel reset through the nRF, backlight GPIO10, touch in the panel (0x3B), 32 MB flash, 8 MB octal PSRAM, 5 keys, GPS opened at 115200 |

```sh
BOARD=c706 tools/build_podman.sh                 # or: idf.py -B build-c706 -DBOARD=c706 build
BOARD=c706 tools/make_release.sh                 # c706-oss-<version>.zip
tools/flash_poc.py -p /dev/ttyACM0 --board c706  # picks build-c706/c706_oss.bin
```

`-DBOARD=<board>` adds `sdkconfig.defaults.<board>` (flash size, PSRAM mode,
partition table, the board choice) on top of `sdkconfig.defaults` and keeps
its own `sdkconfig.<board>`. The C706 keys follow the vendor's map: key 0
click = lap / hold = power off, key 2 = start / pause (hold = end ride),
key 3 / 4 = next / previous page; in the menus 0 = back, 2 = select,
3 / 4 = down / up. The analysis behind the port is in `docs/HARDWARE.md`,
section *Magene C706*; the build runs on a real C706 (display, touch, nRF,
keys, eMMC, GPS verified on 2026-09-19).

Console logs go to the S3's USB-Serial-JTAG (the USB-C port):
`idf.py -p /dev/ttyACM0 monitor`.

## Flash from the source tree

The bootloader, partition table, NVS (contains the phy calibration and the
vendor's device config) and the nRF are untouched. Only the `ota_0` app slot
is overwritten and `otadata` is erased so the bootloader boots `ota_0`.

```sh
# put the device into download mode (GPIO0 low at reset), then:
tools/flash_poc.py -p /dev/ttyACM0            # backs up ota_0 + otadata first (--full-backup for all 16 MB)
tools/flash_poc.py -p /dev/ttyACM0 --dry-run  # just show the partition table
```

To hand a build to someone else: `tools/make_release.sh` zips the binary,
the flash helper and [`FLASHING.md`](FLASHING.md) (short
instructions for the recipient: `pip install esptool`, run the helper).

Restore the vendor app:

```sh
tools/flash_poc.py -p /dev/ttyACM0 --restore path/to/vendor_ota_0.bin
```

(`vendor_ota_0.bin` = the `ota_0` partition dumped from your device, e.g.
with `esp32_image_parser.py dump_partition`.)

## Console and developer console

Console: `tools/serial_log.py /dev/ttyACM0 20 --reset` (inside the IDF
container, or anywhere with pyserial) prints the boot log and every non-periodic
nRF frame.

Developer console (same port, `main/devcon.c`): `tools/devcon.py /dev/ttyACM0
key 0 1` clicks key 0, `... tap 120 160` touches the screen, `... swipe 200 160 40 160` drags (a page swipe), `... shot out.png`
saves a screenshot, `... ls` lists the ride files and `... get
/sdcard/c606oss/<name>.fit` copies one to the host (no need for USB storage
mode), `... put local.gpx /sdcard/c606oss/routes/x.gpx` copies a file to the
card (e.g. a route), `... mv a b` renames a file on the card, `... pos 55.03 82.92` centres
the map page on a position without a GPS fix (`pos` alone: back to the GPS),
`... zoom 13` sets the map zoom, `... layers 3ffff` sets the map layer mask
(hex, bit = layer group in `mapview.c`, saved), `... theme light` / `dark`
switches the theme until the next reboot or auto switch, `... shift 2 3 2 11` feeds a synthetic ANT+
shifting page (front gear 2 of 2, rear 3 of 11; `shift off` forgets the
drivetrain), `... di2 2 3 65` and `... di2 speeds 2 11` do the same for the
Di2 pages, `... sim 55.03 82.92 45 30 60` simulates a
GPS receiver riding from that position on heading 45° at 30 km/h for 60 s
(real RMC/GGA sentences through the real parser; `... nmea off` hands the
GPS back to the receiver), and `... script "key 0 1" "sleep 0.5" "shot
a.png"` chains them (the device log keeps printing during `sleep`). Useful for
exercising the UI without touching the device.

## FIT files

A ride is written to `/sdcard/c606oss/<local date-time>.fit` by `main/tracklog.c` on top
of the small encoder in `main/fit.c`: `file_id`, `file_creator`,
`software`, `device_info` for the unit and the paired ANT+ sensors,
`user_profile`, `zones_target`, `sport`, `bike_profile` (wheel size, bike
weight and, with an electronic shifting sensor, the number of front and
rear gears), a `course` naming the loaded route, timer start/stop events
around pauses, a `front_gear_change` / `rear_gear_change` event for the
starting gear and for every shift, one `record` per second with position, altitude,
distance, speed — wheel sensor if live, else GPS —, HR, cadence, power,
temperature, grade (slope over the last ≥ 30 m, barometric altitude when
the nRF's pressure is live, else GPS) and the calories so far, a `lap`
per lap with its averages/maxima, calories, work, moving time, average
grade and time in HR / power zones, then `session` and `activity`; fields
without data carry the FIT "invalid" value — this is everything the
vendor's own files carry except the ClimbPro events and the sensors'
manufacturer / battery details.

## Configuration storage

The configuration lives in the NVS partition, namespace `c606oss` (the
vendor's entries are untouched); defaults are in `config_defaults()`.
New settings are appended to `app_cfg_t`; older blobs are upgraded in
place (`k_len_by_version` in `config.c` lists each version's blob size,
padding included — the static assert reminds you to extend it).
Layouts are the table in `main/layouts.c`.

## Known unknowns / risks

* The USB PHY mux (`RTCCNTL.usb_conf.sw_usb_phy_sel`) survives software
  resets. The firmware puts it back to USB-Serial-JTAG at boot and before
  leaving USB mode; an older build that did not do this needed the 3-button
  reset to recover.
* The eMMC holds the vendor's data (maps, fonts, ride files, AGNSS). Nothing
  outside `/sdcard/c606oss/` is touched, but treat it with care.
* If ANT+ sensors stop connecting at all (no status broadcasts, no
  "connected"), the nRF's ANT stack is wedged: hold all three buttons
  (hardware reset). The firmware avoids the known trigger (connecting during
  the first seconds after the power-on command).
* **Long press on key 0** makes the vendor firmware shut down; the nRF may do a
  hard power-off on its own regardless of what the ESP32 does.
* Sensor stream (IMU, barometer) decoding in docs/HARDWARE.md is unverified guesswork.
* The command to set the nRF RTC is unknown, so a wrong RTC (seen on a
  Geoid CC700 Pro) stays wrong after power cycles until GPS corrects it.

## Code layout

```
main/board.h       pins, bus settings, protocol constants (from RE)
main/lcd.c         i80 bus + ST7789 init + async bitmap push
main/ui_port.c     LVGL 9 display driver, tick, render task, lock
main/ui.c          pages: status / ride / configured data pages, menu hook
main/datapage.c    renders one configured page (cells, auto font size)
main/layouts.c     the cell layouts ("fences" 1 … 12)
main/fields.c      data field catalogue (name, unit, current text)
main/config.c      page configuration + settings, persisted in NVS
main/menu.c        settings menu (pages, layout picker, field editor, ...)
main/theme.c       dark / light colour theme (shared styles)
main/stats.c       session statistics (min/max/time-weighted avg, staleness)
main/shifting.c    electronic shifting (ANT+ profile and Shimano Di2): gear, gear counts, battery
main/devcon.c      developer console: key/tap injection, screenshots, file transfer
tools/devcon.py    host side of the developer console
main/backlight.c   LEDC PWM
main/nrf_link.c    UART framing, CRC16, TX helpers, key decoding
main/gps.c         UART0 NMEA reader with baud probing
main/sdcard.c      eMMC mount (SDMMC 4-bit)
main/tracklog.c    ride recorder: FIT activity file (records, laps, session)
main/fit.c         minimal FIT encoder (definitions, data messages, CRC)
main/utc.c         wall clock from the nRF RTC or the GPS date
main/sun.c         sunrise / sunset for the last GPS position (NOAA solar equations)
main/health.c      rider profile (BMI, BMR, max HR / LTHR estimates), calories, HR and power zones, time in zones
main/ride.c        idle / riding / paused state machine
main/mapfile.c     Mapsforge binary map reader (header, tile index, way decoding)
main/mapview.c     map page: render task, rasteriser, canvas, zoom buttons, route and trail overlays
main/route.c       GPX track/route loader for the map page, position on the route (also loads a recorded ride)
main/trail.c       the path ridden so far (GPS fixes of the ride), drawn in blue on the map
main/fitread.c     FIT activity reader: records for the track, session / lap summary
main/history.c     ride history: lists our and the vendor's FIT files, delete
tools/mapdump/     host build of mapfile.c: dump or render a tile of a .map file
main/trip.c        distance (wheel sensor or GPS) and auto laps
main/usb_msc.c     TinyUSB mass storage over the eMMC (esp_tinyusb)
main/touch.c       FT6336 / CST328 touch controller over I2C
main/ant.c         ANT+ channel control + HR/speed/cadence/power page decoding
main/sensor_list.c reads the vendor's paired-sensor JSON (imported once)
main/main.c        glue: frame decoding -> UI, key actions, handshake
tools/flash_poc.py flash/restore helper
docs/HARDWARE.md   reverse-engineering notes with addresses
```
