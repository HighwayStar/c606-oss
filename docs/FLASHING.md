# Trying c606-oss on your Magene C606 / C706

You get a zip with:

* `c606_oss.bin` (or `c606pro_oss.bin` / `c706_oss.bin` — check that the
  image matches your device, they are not interchangeable) — the firmware image
* `flash_poc.py` — flash / backup / restore helper (a thin wrapper around esptool)
* `VERSION.txt` — which commit the image was built from
* this file

Nothing else on the device is touched: the vendor's bootloader, partition
table, settings (NVS), the nRF coprocessor and the data on the built-in
eMMC (maps, rides) stay as they are. The helper backs up the vendor
firmware before it writes anything, and you can put it back at any time.

## Needs

* Python 3 and esptool: `pip install esptool` (or `python -m pip install esptool`)
* a USB-C **data** cable; the C606 shows up as a serial port when it is
  switched on (`/dev/ttyACM0` on Linux, `COM<n>` on Windows — see Device
  Manager, `/dev/cu.usbmodem*` on macOS). On Linux add yourself to the
  `dialout` (Debian/Ubuntu) or `uucp`/`dialout` (Fedora/Arch) group, or run
  with `sudo`.

## Flash

Unzip, switch the device on, then in the unzipped directory:

```sh
python flash_poc.py -p /dev/ttyACM0          # Windows: -p COM5, macOS: -p /dev/cu.usbmodemXXXX
```

The helper

1. reads the partition table from the device and prints it,
2. **backs up the vendor app** to `backup-<date>-ota_0.bin` (~7.6 MB, about
   a minute) and `backup-<date>-otadata.bin` — **keep these files**, they are
   the only way back,
3. writes the image into the same `ota_0` slot, erases `otadata` so the
   bootloader boots that slot, and resets the device.

First boot shows the idle screen with the clock. Hold the power key (key 0)
once and cancel the "Power off?" popup — that starts the nRF's
temperature/pressure stream, which is otherwise off after a software reset.

Options: `--dry-run` only prints the partition table; `--full-backup` dumps
all 16 MB instead of just the two partitions; `--no-backup` skips the backup
(only if you already have one).

## Using it

* Keys (C606): **0** = next page, hold = power off; **1** = lap; **2** = start
  ride / pause, hold = end ride. C706: **0** = lap, hold = power off; **2** =
  start ride / pause, hold = end ride; **3** / **4** = next / previous page.
  The gear icon on the touch screen opens the
  settings (pages, layouts, fields, sensors, theme, map layers, route…).
* Rides are recorded as FIT files in `c606oss/` on the eMMC.
* **Maps**: the map page uses the vector maps already on your device
  (`MAP/*.map`, the ones the Magene app downloaded). If the eMMC has no map
  for your area, download one with the vendor app before flashing, or copy a
  `.map` file into `MAP/` in USB storage mode.
* **Routes**: copy `.gpx` tracks/routes into `c606oss/routes/` and pick one
  in Settings → Route.
* **USB storage mode** (Settings → System → USB storage) shows the eMMC on
  the PC as a removable disk for copying maps, routes and FIT files. The
  serial port is gone while in this mode — eject the disk and tap *Reboot*.

Everything is described in more detail in the project README.

## Back to the vendor firmware

```sh
python flash_poc.py -p /dev/ttyACM0 --restore backup-<date>-ota_0.bin
```

This writes the backup into `ota_0` and erases `otadata` again; the vendor
firmware starts with all its settings intact.

## If something goes wrong

* **esptool cannot connect**: make sure the device is on (press the power
  key), try another cable/port (some USB-C cables are charge-only), close
  any other program that has the port open, or hold **all three buttons**
  for a few seconds — that hardware-resets the ESP32 — and retry immediately.
* **The screen stays dark / the device seems dead**: hold all three buttons
  (hardware reset). If it still doesn't come up, `--restore` as above; the
  bootloader is untouched, so esptool always works.
* **"does not fit into the app slot"** / no `ota_0` found: your device has a
  different partition layout than the ones seen so far — send the
  `--dry-run` output to the project, don't force it.
* **Temperature/pressure show `--.-`**: hold the power key once and cancel
  the popup (see above).
* **Without the helper** (plain esptool, no backup) the equivalent on the
  vendor layout is:

  ```sh
  python -m esptool --chip esp32s3 -p PORT write_flash 0x20000 c606_oss.bin
  python -m esptool --chip esp32s3 -p PORT erase_region 0xd000 0x2000
  ```

  Check the offsets against `--dry-run` first: `0x20000` is `ota_0`,
  `0xd000` is `otadata` on the vendor table (both C606 and C706).
