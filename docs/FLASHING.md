# Trying c606-oss on your Magene C606

You get a zip with `c606_oss.bin` (the firmware), `flash_poc.py` (flash
helper) and this file. Nothing else on the device is touched: the vendor's
bootloader, settings and the data on the built-in eMMC (maps, rides) stay,
and you can put the original firmware back at any time from the backup the
helper makes.

## Needs

* Python 3 and esptool: `pip install esptool`
* a USB-C data cable; the C606 shows up as a serial port when it is switched
  on (`/dev/ttyACM0` on Linux, `COM<n>` on Windows, `/dev/cu.usbmodem*` on macOS)

## Flash

```sh
python flash_poc.py -p /dev/ttyACM0          # Windows: -p COM5
```

The helper reads the partition table, **backs up the vendor app** to
`backup-<date>-ota_0.bin` (~7.5 MB, about a minute — keep this file!), writes
`c606_oss.bin` into the same slot and resets the device. First boot shows the
idle screen with the clock; hold the power key once and cancel the popup so
the nRF starts its temperature/pressure stream.

Only `-p` is required; `--dry-run` just prints the partition table.

## Back to the vendor firmware

```sh
python flash_poc.py -p /dev/ttyACM0 --restore backup-<date>-ota_0.bin
```

## If something goes wrong

* esptool cannot connect: make sure the device is on (press the power key),
  try another cable/port, or hold **all three buttons** for a few seconds —
  that hardware-resets the ESP32 — and retry.
* The screen stays dark or the device seems dead: hold all three buttons
  (hardware reset), then `--restore` as above.
* Without esptool auto-reset: `python -m esptool --chip esp32s3 -p PORT
  write_flash 0x20000 c606_oss.bin` followed by `python -m esptool --chip
  esp32s3 -p PORT erase_region 0xd000 0x2000` does the same as the helper
  (minus the backup).

Keys: 0 = next page / power (hold), 1 = lap, 2 = start ride / pause (hold:
end ride). Settings are behind the gear icon (touch), everything is described
in the project README.
