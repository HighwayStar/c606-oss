# Magene C606 hardware notes (from firmware reverse engineering)

Source: vendor firmware C606 v1.711 (`ota_0-606-1.711.elf`, Ghidra, Xtensa LE).
Addresses below refer to that image. Vendor function names come from log
strings (`__func__` arguments to the logger), not from symbols.

## SoC / software stack

| Item | Value |
|---|---|
| SoC | **ESP32-S3** (`//IDF/components/esp_system/port/soc/esp32s3/`, app header chip_id 9) |
| Framework | ESP-IDF 5.3+ (`esp_driver_gpio`, `esp_driver_ledc`, `esp_driver_uart`, `esp_driver_sdmmc`, `esp_driver_i2c`), FreeRTOS |
| UI | LVGL 8.x (`lv_disp_drv_t` with `flush_cb` at +0x14, `user_data` at +0x3c) |
| Flash | 16 MB, DIO, 80 MHz (vendor app image header byte 3 = `0x4f`) |
| PSRAM | **embedded 2 MB Quad** (esptool: "Embedded PSRAM 2MB (AP_3v3)"; IDF: "Found 2MB PSRAM device, 80MHz, memory test OK"), chip rev v0.2, QFN56 |
| Partitions (read from device) | `nvs` 0x9000/0x4000, `otadata` 0xd000/0x2000, `phy_init` 0xf000, `coredump` 0x10000/0x10000, **`ota_0` 0x20000**, `ota_1` 0x760000, each 0x73A000 |
| Co-processor | Nordic nRF ("Minor MCU", `mSysTimeStampFromNrf`) on UART2. Handles buttons, power, charging, sensors, RTC. Upgradable from the ESP32 ("Upgrade Slave MCU"). |
| USB | TinyUSB CDC (+ SD as MSC), so the USB-C goes to the S3 native USB pins (19/20). USB-Serial-JTAG console works on the same pins. |

## Display

`MidLcdInit()` @ `0x42031740` (`Modules/Middlewares/MidLcd/MidLcd.c` lines 0x16d..0x22f)

* Panel: ST7789, **240x320**, 16 bpp, via `esp_lcd` **i80 (8080) bus, 16-bit wide**, LCD_CAM peripheral.
* `esp_lcd_i80_bus_config_t`:
  * `dc_gpio_num = 40`, `wr_gpio_num = 3`, `clk_src = PLL_F160M (6)`
  * `data_gpio_nums = {4, 38, 5, 37, 6, 36, 7, 35, 8, 34, 9, 33, 10, 47, 11, 48}`
  * `bus_width = 16`, `max_transfer_bytes = 28800` (= 240 * 60 lines * 2), `sram_trans_align = 4`
* RD (`GPIO39`) is configured as a plain output and driven high.
* `esp_lcd_panel_io_i80_config_t`: `cs = 2`, `pclk = 15 MHz`, queue depth 10, cmd/param 8 bit,
  `dc_levels = {idle 0, cmd 0, dummy 0, data 1}`, `flags = 0`.
* `esp_lcd_panel_dev_config_t`: `reset_gpio_num = -1` (no ESP-controlled reset),
  `rgb_ele_order = RGB`, `bits_per_pixel = 16`.
* After init the vendor calls `invert_color(true)`, `swap_xy(false)`, `set_gap(0,0)`.
* LVGL draw buffers: two 28,800-byte `heap_caps_malloc(.., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`; `hor_res=240, ver_res=320`; flush callback just calls `esp_lcd_panel_draw_bitmap` (no byte swap).
* LVGL tick: `esp_timer` periodic 2 ms ("lvgl_tick").

### Panel init sequence (`mg_panel_st7789_init` @ `0x42031c48`)

Reset (`0x42031bf0`): with no reset GPIO it sends `0x01` (SWRESET) and waits 20 ms. Then:

```
11            ; SLPOUT, delay 120 ms
36 <madctl>   ; 0x00 for RGB order
3A 55         ; 16 bpp
B2 0C 0C 00 33 33
B7 74
BB 1E
C0 2C
C2 01
C3 10
C4 20
C6 0F
D0 A4 A1
E0 F0 06 0B 06 07 25 34 44 4A 38 14 13 2E 34
E1 F0 0C 10 0A 09 06 33 43 49 36 12 14 2A 32
E9 11 11 03
21            ; INVON
29            ; DISPON
2C            ; RAMWR
```
(1 ms delay after each command; constants at `0x3c373760`.)

### Backlight (`MidLcdPwmInit` @ `0x4203162c`)

LEDC low-speed, timer 0, **10-bit**, **20 kHz**, `LEDC_AUTO_CLK`; channel 0 on **GPIO45**, duty 0 at init, active high.

### LCD power

There is a message to the nRF `E2 02 03 00 00 02 00 00` (type 2, cmd 0x10) sent
from the LCD module (`0x42031a78`) followed by 200 ms delay and a full panel
re-init. This is only done when NVS `Res1Page11` byte 3 != 2 (HW variant) and
looks like "power-cycle the LCD" on wake-up. The PoC does not send it; the
panel is already powered when the ESP32 boots.

### Touch

`InitI2CBus` (`0x420321f4`): **I2C0, SDA GPIO21, SCL GPIO12**, internal
pull-ups, glitch filter 7; devices added at 400 kHz (`I2CManagerAddDev`).
`FUN_42031b14` probes **0x38** first, then **0x5A**; no reset/interrupt GPIO,
LVGL polls it from `touch_driver_read`. Touches are ignored while the screen
is off (`FUN_4216681c`).

* 0x38 = **FocalTech FT6336** (this unit: reg 0xA8 vendor 0x11, 0xA3 chip 0x64, 0xA6 fw 0x09).
  Init: read 0xA8, write reg 0x00 = 0, read 0xA6, 0xAF. Read: reg 0x02 -> 5 bytes
  `[count, XH, XL, YH, YL]`, pressed when count == 1, X = (XH&0x0F)<<8|XL, Y likewise.
* 0x5A = **Hynitron CST328** (other HW revision): 16-bit registers, probe by reading
  4 bytes at 0xD045; data = 7 bytes at 0xD000, pressed when `b0 & 0x0F == 6`,
  X = b1<<4 | b3>>4, Y = b2<<4 | b3&0x0F; after each read write `D0 00 AB`.

Coordinates map 1:1 onto the 240x320 panel (measured: top-left ≈ 11,27,
bottom-right ≈ 219,293), no swap or mirror.

## nRF co-processor link

`MidCommInit()` @ `0x42045f44`:

| idx | UART | pins | baud | used for |
|---|---|---|---|---|
| 1 | UART2 | TX **GPIO42**, RX **GPIO41** | **115200** | nRF (keys, power, sensors) |
| 0 | UART0 | TX GPIO1, RX GPIO0 | 921600 | GPS module |

RX buffer 0x400, TX 0x108, no event queue. A 10 ms timer (`UartRecvTimer`,
`UartDataSaveToRingBuffer` @ `0x42051170`) moves bytes into a ring buffer;
the receive task (`0x42051330`) frames them.

### Frame format

```
 0    1    2    3    4      5     6 .. 6+len-1   N+2  N+3
 A5   N    6F   DIR  TYPE   CMD   payload        CRClo CRChi
```
* `N = len + 4`; total frame = `N + 4`; vendor rejects `N >= 0x85`.
* `DIR`: ESP32 sends `F1`; the value in nRF frames is not checked.
* `TYPE`: must be 1..5. ESP32 uses 1 = query (`SendCheckPowerOnReasonCmd`) and 2 = set.
* `CRC16`: `FUN_4222c01c` = **CRC-16/XMODEM** (poly 0x1021, init 0, no reflection,
  check `0x31C3`). Computed over bytes `[0 .. N+1]`, stored little-endian.
  Note: Ghidra's decompiler drops the two `loop a10,8` tail blocks and shows
  only 2 augmenting shifts — read the disassembly, not the C.
* TX builder: `FUN_42051260(type, cmd, payload, len)`.

### CMD dispatch (`0x420521c4`, `cmd = frame[5]`)

| cmd | handler | notes |
|---|---|---|
| 0x00 | `0x420519ac` | payload[0]=`'S'`(0x53): RTC time from nRF (`sec,min,hour,day,mon,year`), `'R'`(0x52): 3-byte value |
| 0x01 | `0x42051bd8` | sensor/BLE-ish: sub 0x01 (version bytes), 0x17 (connection state, "Com Disconn"), 0x24/0x25/0x26, 0x27/0xF2 |
| 0x10 | `0x42051e34` | **system**: see below |
| 0x0B,0x11,0x22,0x23,0x28, 0x78..0x7B, 0x80 | generic | 8-byte payload queued as type 5 |

### ANT+ sensors (verified on hardware)

The nRF is the ANT radio only; the ESP32 opens channels and decodes the
raw ANT+ data pages itself (`AntPageEventHandler` @ `0x421b8bd4`,
`AntSendCmd` @ `0x420595c4`, `StartAntScanDrive` @ `0x421b8fc8`,
`StartAntDisConnectDrive` @ `0x421b91ec`). Device types are the ANT+ ones:
11 power, 17 fitness equipment, 34 shifting, 35 light, 40 radar, 120 HR,
121 speed+cadence, 122 cadence, 123 speed, 128 (Di2, treated as shifting).

| dir | type/cmd | payload | meaning |
|---|---|---|---|
| ESP->nRF | 2 / `0x01` | `17 <devtype> <num lo> <num hi> <trans> 00 19 00` | open channel (search timeout 0x19 = 25 s) — vendor log `AntSnd:2,1,P=…` |
| ESP->nRF | 2 / `0x01` | `17 <devtype> 00 00 00 01 00 00` | close channel |
| nRF->ESP | 5 / `0x01` | `00 00 …` | command ack |
| nRF->ESP | 4 / `0x01` | `17 <devtype> <num lo> 00 <trans> <st> 00 00` | channel status: 3 connected, 5 search timeout, 4 not connected — **also broadcast for every known channel every 5 s as a status report**, so 4 alone must not trigger a reconnect (doing so drops the live channel) |
| ESP->nRF | 2 / `0x10` | `E1 02 00 <T lo> <T hi> FF 00 00` | ANT scan for T seconds (T = 0 stops) |
| nRF->ESP | 4 / `0x10` | `F3 03 xx <devtype> <num LE16> <trans> <rssi> <flag>` | scan result; `flag` 0 = scan end |
| nRF->ESP | 4 / `<devtype>` | 8-byte ANT+ data page | sensor data, one frame per received page (~4 Hz) |

Observed pages: HR page 4 `00 <prev beat time> <beat time> <count> <bpm>`
(bit 7 of byte 0 is the ANT toggle bit); speed 123 / cadence 122
`ff ff ff <event time 1/1024 s LE> <revs LE>` with `ff` in the reserved bytes;
page 1 with zeros while idle. The nRF keeps the previously connected device
list across ESP32 resets and reports them in its 5 s status broadcast.

Paired sensors live in `/sdcard/CONFIG/sensor_list.json`
(`ConnType` 0 = ANT+, 1 = BLE; `DevType` = vendor index 0 HR, 1 cadence,
2 speed, 3 spd+cad, 4 power, 5 trainer, 6 radar, 7 shifting, 8 light;
`DevID` = `<device number>-<transmission type>`). BLE sensors are handled by
the ESP32's own Bluedroid stack (`kaka:` log lines), not by the nRF.

Pitfalls found on hardware:
* The nRF keeps its ANT device list across ESP32 resets (it stays powered
  when the ESP32 is reset or even powered off) and reports it every 5 s;
  after our power-on command it tears the channels down and lists them
  with status 4. **A connect sent within the first ~6 s after the power-on
  command is accepted (status 3, pages for a few seconds) and then killed at
  the nRF's next 5 s tick**; doing this repeatedly leaves the ANT stack
  completely silent until the three-button hardware reset. Connecting ~8 s
  after boot (after that tick) is stable across any number of ESP resets.
  The vendor only ever connects once per power-on so it never hits this.
* The close command (`17 <type> 00 00 00 01`) sent at boot had the same
  effect (silent stack). The vendor closes channels only in its re-pairing
  flow (close -> scan -> stop scan -> open).
* Repeating `SendPowerOnCmd` every 5 s (an earlier keep-alive) turned out to
  be harmless; the vendor sends it only until acknowledged.
* Scan: results `F3 03 xx <type> <num> <trans> <rssi> <flag>` appear within
  1-3 s (measured RSSI -37..-61 dBm); a `17 00 .. 05` then `17 00 .. 04` event
  marks the scan's end; `flag`=0 is the ScanEnd marker.
* Decoded live: HR page 4 (72-85 bpm), cadence 122 (26 rpm from slow crank
  turns via event-time/rev deltas); speed 123 uses the same math.

### cmd 0x10 payloads (nRF -> ESP32)

| payload[0] | meaning |
|---|---|
| **0x49** | **button event**: `[1]` key index (0..2 stored, up to 4 accepted), `[5]` aux, `[6]` event value. Queued as "type 0" -> `ArmSendBtnEvent` -> `KeyQueueReceive`. |
| 0xE2 | reply to E2 control: `[1]=2` -> power-on reason in `[5]` (4 = manual power-on, 5/6 = charger) |
| 0xF0 | `[1]=2`: 1-byte status (type 3) |
| 0xF1 | sensor stream, ~5 Hz, all subtypes back-to-back (see below) |
| 0xF3 | `[1]=3`: type 7 record |

Key event values seen in `KeyQueueReceive` (`0x42056ad0`):
`4` = "Long Press Start" (vendor then synthesizes value `6` every 300 ms while
held, and reports "Hold Up" when the next non-4 event arrives). In the main
state machine key 0 with value 1 is logged as "A button pressed!" and key 0
with value 4 triggers shutdown.

**Measured on hardware (PoC, 2026-09-17)**, frames are `49 <idx> 00 00 00 00 <evt> 80`:

| evt | meaning | timing |
|---|---|---|
| 1 | short press ("click") | one frame, sent on release |
| 4 | long press | first frame ~0.5 s after press-down, then repeated by the nRF every ~250 ms while held |
| 5 | release after a long press | one frame |

The three physical keys are idx 0, 1, 2. (Value 6 is generated inside the
vendor firmware, never seen on the wire.)

### Observed unsolicited / reply traffic (PoC log)

Every 300 ms:
* `A5 0C 6F F1 04 00 52 FF FF FF F7 10 64 FF` — cmd 0 `'R'`: battery, `u16 @4` = mV (0x10F7 = 4343), `@6` = percent (0x64)
* `A5 0C 6F F1 04 10 F0 02 00 00 00 00 00 00` — status byte `@2` (0 while on battery; probably charger state)

In reply to `SendPowerOnCmd`:
* `A5 0C 6F F1 05 10 E2 02 01 00 00 00 00 00` — ack (type 5)
* `A5 0C 6F F1 04 10 E2 02 00 00 FF 04 00 FF` — power-on reason `@5` = 4 (manual)
* `A5 0E 6F F1 04 01 01 01 00 00 00 00 00 00 02 13` — nRF firmware version, vendor parses `@7,@8,@9` -> 0 / 2 / 19

Every second (repeated 5x):
* `A5 0C 6F F1 04 00 53 FF ss mm hh dd MM yy` — cmd 0 `'S'`: RTC, `sec, min, hour, day&0x1f, month-1, year-1900` (observed `.. 3A 0C 91 08 7E` = 2026-09-17 12:58 UTC, correct).

Sensor stream `cmd 0x10, F1 <sub>`, 5 Hz, together with the RTC frames
(`00 53`, 5 Hz). **The nRF starts it only after a long press of the power
key** (key 0) — that is part of its own power-on gesture, so on the vendor
it is always running; after an ESP-only reset (esptool, `esp_restart`) it is
off until the key is held again. Nothing the ESP32 can send starts it
(tested: power-on repeats, `E2 01 18/19`, the `24 01` slave-mode query,
GPS power, motion, ANT activity — none matter). Once started it runs
until the nRF is power-cycled. Interpretation of the values, from the
vendor's math (ImuDataInit / FUN_4221f530 / FUN_4221f708 / SetMagnOtpData):

| sub | payload[2..7] | reading |
|---|---|---|
| 00 | u8 @8, u8 @9, u32 @2, u16 @6 | magnetometer OTP calibration (`SetMagnOtpData`) |
| 01 | 3 x int16 | accelerometer, value/32768 * 8 g (32 g on HW variant `Res1Page11`); ≈ 1 g at rest |
| 02 | 3 x int16 | gyro, value/32768 * 2000 dps |
| 03 | int16 @2, u32 @4 | temperature in 0.01 °C (`AD 0B` = 29.89 °C), pressure in 0.01 Pa (`BC 2B 92 00` = 957.9 hPa). Vendor feeds these into the barometric altitude formula (44330 * (1 - (p/1013.25)^0.19)). |
| 04 | u32 @2, u16 @6 | unknown (`32 00 00 00 77 00`), queued as type 9 |
| 0A | int16 @2 | unknown float source (`F6 0C` = 3318) |

The nRF keeps sending without any ESP32 traffic and does not power the ESP32
down on its own (tested for several minutes with only `SendPowerOnCmd` every 5 s).

### cmd 0x10 payloads (ESP32 -> nRF), all 8 bytes, `E2 grp id 00 00 val 00 00`

| bytes | vendor name |
|---|---|
| `E2 02 00 00 00 01 00 00` (type 2) | `SendPowerOnCmd` — sent every ~1 s in state `INIT_QUERY` until an `E2 02` reply arrives |
| `E2 02 00 00 00 01 00 00` (type 1) | `SendCheckPowerOnReasonCmd` |
| `E2 02 00 00 00 00 00 00` | `SendPowerOffCmd` |
| `E2 02 03 00 00 02 00 00` | LCD power (see above) |
| `E2 02 07 00 00 0X 00 00` | GPS power 0/1/2 |
| `E2 02 08 00 00 01 00 00` | factory init |
| `E2 01 XX 00 00 00 00 00` | LED/misc, XX < 0x1c (0x17, 0x13, 0x18/0x19 seen) |

Power-off (vendor): long press on key 0 -> `_KeyFunc_PowerOffPopUp` (the
open firmware shows the same kind of popup: key 0 / "Off" confirms, anything
else cancels, 8 s timeout) -> after confirmation `SYS_EVENT_POWER_OFF` -> `SendPowerOffCmd` (`E2 02 00 00 00 00 00 00`)
-> the nRF cuts the ESP32's power. The nRF never powers off from the key alone.
**Hardware reset: holding all three buttons makes the nRF reset/power-cycle the
ESP32** (found empirically; a real power-on reset, clears RTC registers).

Boot state machine (`ArmStateProcess` @ `0x420570dc`):
`INIT_QUERY (1)` -> `USER_INIT (3)` -> `FILE_CHECK (4)` -> `USER (9)`;
in `USER` every 10 s it goes through state `0xb` which polls the nRF.
No watchdog behaviour observed so far; the PoC re-sends `SendPowerOnCmd`
every 5 s anyway.

## GNSS receiver

**Airoha AG3352Q**, firmware `AG3352Q_V3.0.2.AG3352_20230717` (from its
`$PAIR021` banner). UART0 @ **921600** 8N1, TX GPIO1, RX GPIO0
(`MidCommInit(0, 921600, 0x1000, 0x400, 1)`; `MidCommBaudrateSwitch` only
runs if the stored baud differs, so 921600 is the module's configured rate).
Plain NMEA (`$GN…` RMC/GGA/GSA/GSV/…, ~20 sentences/s) is on by default.

Power is controlled by the nRF: `E2 02 07 00 00 <v> 00 00` with `v` = 0 off,
1 on, 2 hard reset (`GpsHdRst`, used after 50 ticks without data). The vendor
never sends "on" at boot; the module is already powered.

`GpsPthreadMachine` (`0x42167e90`) command flow for the Airoha (chip type 4/5;
table at `0x3fca499c`, `FUN_421cb228(n)` sends entry `n+0x1f`):
`$PAIR867,0,0` -> ack `$PAIR001,867` -> `$PAIR866,0,0,0` -> `$PAIR490,1` -> `$PAIR491`
-> running; `$PAIR470,0` after an EPO/AGNSS upload (`$PAIR590,…` + binary
`0x4B0/0x4B1/0x4B2` packets built by `FUN_42167c3c` from `/sdcard/…_agnss.bin`).
These only affect EPO/AGNSS behaviour. Reset table (`0x3fca4954`, 6 chip
columns): `$PAIR004/005/006` = hot/warm/cold start. The same driver also
carries CASIC (`$CFGMSG…`, chip 0) and Unicore-style (`$CCMSG…`) tables for
other hardware revisions.

## Other peripherals

* **"SD card" = on-board 4 GB eMMC** (`004GA1`, 3776 MB, MMC, 20 MHz): SDMMC host slot 1, 4-bit,
  CLK 13, CMD 14, D0 16, D1 17, D2 18, D3 15, internal pull-ups (`MidVFSMount`, host =
  `SDMMC_HOST_DEFAULT`, `max_files` 17, 16 KB allocation unit, **format_if_mount_failed = true** in
  the vendor code). FAT with long names, mounted at `/sdcard`. Vendor directory tree:
  `System Volume Information/ FONT/ MAP/ BOOT/ SD/ APP/ GPS/ FITS/ COURSE/ NAVIGATION/ EPHEMERIS/ CONFIG/`
  (`FITS/` = ride recordings, `EPHEMERIS/` = AGNSS/EPO files for the GNSS, `MAP/` = map tiles).
  Full root as seen over USB: `ABNORMAL APP AUDIO BOOT CONFIG COURSE EPHEMERIS FITS FONT GPS
  GROUPRIDE LOG MAP ModuleDataTest NAVIGATION NOTIFY REGION SD SEGMENT SMART TMP USER WIFI`,
  plus `hello.txt` ("hello emmc!") and an empty `<date>.logg`.
  * `LOG/<yyyymmdd>.log` — the vendor's full text log (2–3 MB/day, the same strings we see in
    Ghidra with live values). Best source for protocol behaviour.
  * `APP/<timestamp>_<n>/N21_V<ver>_A.bin` — OTA packages for the ESP32 (V1.409 … V1.956, the
    latter from 2026-07); `N22_V1.902_A.bin` alongside is most likely the nRF firmware. Not raw
    ESP images: obfuscated with a repeating 4-byte pattern (`9a 92 45 42` where padding would be
    zero) — looks like a 4-byte XOR. Not decoded yet.
  * `FITS/FIT/<unix time>.fit` — ride recordings (Garmin FIT).
  * `CONFIG/sensor_list.json` (paired sensors), `ProductInfo.config`, `FileVer.info`.
  * `EPHEMERIS/<unix time>_agnss.bin` — AGNSS/EPO data for the Airoha GNSS.
  * `MAP/*.map` — **vector maps in the plain [Mapsforge binary map format](https://github.com/mapsforge/mapsforge/blob/master/docs/Specification-Binary-Map-File.md), version 3**
    (magic `mapsforge binary OSM`, "created by mapsforge-map-writer-0.18.0" / "-0.23.0"). The vendor
    generates them with `osmosis --read-pbf-fast … --bounding-polygon … --mw file=… tag-conf-file=tm.igsport.xml
    zoom-interval-conf=5,0,7,10,8,11,14,12,15 threads=6 type=hd`, i.e. three zoom intervals with base
    zooms 5 (levels 0–7), 10 (8–11) and 14 (12–15), tile size 256, no POIs at all, 17–21 way tags
    (`highway=*`, `natural=water`, `natural=coastline`, `lock=yes`), street names and refs present, no
    debug signatures. The 0–7 interval holds only coastlines (China) or nothing (Siberia); water
    appears from zoom 12. Largest records seen: base-10 tile 407 KB, base-14 tile 174 KB, single way
    3 KB / 1031 nodes (China). Coordinates: `mars_china_*.map` is GCJ-02 shifted (the Forbidden
    City moat lands ~700 m NW of its WGS-84 position), `mars_russia_siberian-fed-district_*.map` is
    plain WGS-84 (verified against OSM: площадь Калинина, Novosibirsk). `map.trans` is a plain text
    region-name translation table. `*.etu` (e.g. `bg_china_big_v….101.etu`) are encrypted/compressed
    background files, format unknown. The reader is `main/mapfile.c` (verified against a Python
    reference decoder on both files, `tools/mapdump/` on the host). Reading needs
    `CONFIG_FATFS_USE_FASTSEEK`: without the cluster link map every backward `fseek` in a 180 MB file
    walks the FAT chain (a 9-tile view took 2.4 s, 80 ms with it).
  The open firmware writes only under `/sdcard/c606oss/`. A copy of everything except
  `MAP/` and `FONT/` lives in `~/devel/magene/emmc/` (outside this repo).

  USB: with `esp_tinyusb` MSC over the same SDMMC card the host sees a 7,733,248-sector
  (3.96 GB) removable disk; the vendor does the same with TinyUSB CDC+MSC.
  Caveat: the S3 internal-PHY mux `RTCCNTL.usb_conf.sw_usb_phy_sel` is an RTC register;
  after TinyUSB used the OTG controller it must be set back to USB-Serial-JTAG
  (`usb_serial_jtag_ll_phy_enable_external(false)`) before a software reset, otherwise
  console and esptool stay dead until a power-on reset.
* **NVS**: standard `nvs` partition; vendor config blob `Res1Page11` (byte 3 = HW variant).
* GPIO43/44 (default UART0 pins) are driven high as outputs on HW variant 2 before LCD init.
