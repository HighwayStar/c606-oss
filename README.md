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
* mounts the on-board 4 GB eMMC (FAT, `/sdcard`) and records CSV tracks to `/sdcard/c606oss/`,
* exposes the eMMC over USB as a mass-storage disk on demand,
* touchscreen (FT6336 over I2C) as an LVGL pointer: on-screen REC / USB buttons,
* ANT+ sensors through the nRF: auto-connects to the sensors paired in the vendor's
  `CONFIG/sensor_list.json` (HR, speed, cadence, power decoded; shows on the ride page),
* session statistics (current / min / max / avg) for every measured parameter —
  GPS speed and altitude, HR, cadence, sensor speed, power, temperature,
  pressure, battery,
* up to 5 user-configurable data pages, Bryton/Magene style: each page has
  one of 18 cell layouts ("fences" 1, 2, 3A … 12) and every cell shows one
  data field (Speed, Max Speed, Avg HR, Time of Day, …). Configured on the
  device in the settings menu (gear icon), saved in NVS,
* developer console on the USB port: inject key/touch events and take
  screenshots from the host (`tools/devcon.py`),
* idle / riding / paused modes: the idle screen (clock, GPS and sensor state,
  START button) is shown until a ride is started; the data pages, the track
  recording and the statistics only run during a ride,
* keys: 0 = next page, 1 = backlight level, 2 = start ride / pause / resume,
  hold 2 = "End ride?" dialog, hold 1 = USB storage mode (hold 1 again to reboot
  out of it), hold 0 = power-off popup.

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

1. Idle screen: clock (nRF RTC + time zone), GPS fix / satellites, paired
   sensor values and a *START RIDE* button. Key 0 switches to the
   status page (battery arc with mV, temperature and pressure, `nRF ok reason 4
   fw 0.2.19`, GPS and eMMC state, three key boxes, event log, heap footer) and
   back. A key box flashes green on a click and stays red while long-pressed.
2. Key 2 (or *START RIDE*) starts a ride: statistics are reset, a CSV track is
   opened in `/sdcard/c606oss/<utc date-time>.csv` (one line per second with a
   fix: position, altitude, speed, course, sats, HDOP, temperature, pressure)
   and the data pages appear ("Page 1" … "Page 5", only the enabled ones; key 0
   cycles). The header shows `▶ h:mm:ss` (session time). Key 2 pauses (`‖`,
   recording and statistics stand still, the time excludes pauses) and resumes.
   Hold key 2 for the *End ride?* dialog: key 2 / *End* closes the track file
   and returns to the idle screen, anything else cancels.
3. Data page cells show a field's name and value (font sized to the cell;
   grey `--` when no fresh reading arrived within a few seconds). Fields are
   the current / min / max / avg of every measured parameter, distance and
   laps (Distance, Laps, Lap Dist, Lap Time, Lap Speed, PreLap Time, PreLap
   Dist), time of day, session time, battery % and satellites. Distance comes
   from the ANT+ wheel sensor when it is live (revs × `ANT_WHEEL_CIRC_M`),
   otherwise from consecutive GPS fixes (moving faster than 2 km/h); a lap
   ends automatically every *Lap length*. The average is time-weighted, so it
   does not depend on how often a sensor reports; cadence and HR ignore zero
   samples. *Reset statistics* in the menu restarts the session by hand.

   **Settings menu** — tap the gear icon in a page header. Touch or keys work
   (key 2 = up, key 1 = down, key 0 = select; selecting the back arrow goes
   back):
   * *Pages* → *Page n* → *Enable*, *Layout* (preview of the page with an
     up/down selector, tick applies), *Fields* (preview of the page: tap a cell,
     or move the yellow frame with the keys and press key 0, then pick a
     category and a field).
   * *Lap length* (tap: +0.5 km, 0.5 … 10 km, then off).
   * *Time zone* (tap: +1 h, wraps at UTC+14 → UTC-12) for the time of day,
     which comes from the nRF's RTC (UTC), GPS as fallback.
   * *Theme*: dark (default) or light, applied immediately (`main/theme.c`:
     shared LVGL styles; status colours are darkened on the light background).
   * *Reset statistics*.
   * *System* → *USB storage* (same as holding key 1), *Power off*, *About*.
   The configuration lives in the NVS partition, namespace `c606oss` (the
   vendor's entries are untouched); defaults are in `config_defaults()`.
   Layouts are the table in `main/layouts.c`.

4. Hold key 1: the eMMC appears on the host as a 3.7 GB USB disk
   (`303a:4002 c606-oss C606 eMMC`, auto-mounted by most desktops). The S3
   has a single USB PHY, so the console and esptool auto-reset are gone
   while in this mode — eject the disk and hold key 1 again to reboot.
5. Hold key 0: "Power off?" popup — key 0 again (or tap Off) powers off via
   the nRF, any other key (or Cancel, or 8 s) dismisses it. Press key 0 to
   power on again.

**Temperature / pressure show `--.-` after flashing?** The nRF only streams
its IMU/baro data after the power key has been *held* (its power-on gesture).
An esptool or software reset skips that, so hold key 0 once and cancel the
popup; a normal button power-on doesn't need it.

**If the ESP32 is ever unreachable** (no USB device, screen frozen): hold all
three buttons for a few seconds — the nRF performs a hardware reset / power
cycle of the ESP32 regardless of what the firmware is doing.

Console: `tools/serial_log.py /dev/ttyACM0 20 --reset` (inside the IDF
container, or anywhere with pyserial) prints the boot log and every non-periodic
nRF frame.

Developer console (same port, `main/devcon.c`): `tools/devcon.py /dev/ttyACM0
key 0 1` clicks key 0, `... tap 120 160` touches the screen, `... shot out.png`
saves a screenshot, and `... script "key 0 1" "sleep 0.5" "shot a.png"` chains
them. Useful for exercising the UI without touching the device.

If the screen stays dark: check the backlight (GPIO45) first — the bars are
drawn before it is enabled, so a dark-but-flickering panel means the i80 bus
works. If colours are swapped (red <-> blue) build with
`-DLCD_MADCTL=0x08` (BGR bit) in `main/CMakeLists.txt`.

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

## Layout

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
main/devcon.c      developer console: key/tap injection, screenshots
tools/devcon.py    host side of the developer console
main/backlight.c   LEDC PWM
main/nrf_link.c    UART framing, CRC16, TX helpers, key decoding
main/gps.c         UART0 NMEA reader with baud probing
main/sdcard.c      eMMC mount (SDMMC 4-bit)
main/tracklog.c    CSV track recorder
main/ride.c        idle / riding / paused state machine
main/trip.c        distance (wheel sensor or GPS) and auto laps
main/usb_msc.c     TinyUSB mass storage over the eMMC (esp_tinyusb)
main/touch.c       FT6336 / CST328 touch controller over I2C
main/ant.c         ANT+ channel control + HR/speed/cadence/power page decoding
main/sensor_list.c reads the vendor's paired-sensor JSON
main/main.c        glue: frame decoding -> UI, key actions, handshake
tools/flash_poc.py flash/restore helper
docs/HARDWARE.md   reverse-engineering notes with addresses
```
