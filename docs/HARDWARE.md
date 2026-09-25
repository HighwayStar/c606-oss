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

### Electronic shifting (ANT+ device type 34)

The vendor keeps two modules under `Modules/Middlewares/MidAntDeviceManage`:
`ant_shft/ant_shft.c` for the ANT+ shifting profile (device type 34, used by
SRAM AXS / eTap, Magene QED and Shimano in its ANT+ mode) and `ant_di2/ant_di2.c`
for Shimano's private Di2 stream (type 128, "DI2-%d-%d" in scan results).
Both follow the Nordic ANT+ library shape: a `..._disp_init(p_profile,
evt_handler)` fills a profile object, the library's dispatcher decodes the
raw page into the profile and then calls the application handler with the
page number.

C606 V1.711: dispatcher `FUN_421c2594`, page-1 decoder `FUN_422bddcc`,
application handler `mg_shft_dis_evt_handler` @ `0x421bfba4` (undefined in
Ghidra until a function is created at the `entry` there), profile object
`0x3c6d5650`, init `ant_shft_disp_init` @ `0x421c24bc` from `FUN_421c09dc`.
The C706 V1.729 image has the same code: `FUN_42205110` / `FUN_42348f04`.

Data page 1 ("shift system status"), the only one needed for the gear
display, as the vendor decodes it (`FUN_422bddcc` gets `payload + 1`):

| byte | contents |
|---|---|
| 0 | page number `0x01` (no toggle-bit masking in this profile - pages `0xF0`..`0xF5` exist) |
| 1 | shift / event count (kept by the vendor, never used) |
| 3 | bits 0-4 current **rear** gear, bits 5-7 current **front** gear, both 0-based; all-ones (`0x1F` / `0x7`) = invalid |
| 4 | bits 0-4 **total** rear gears, bits 5-7 **total** front gears |

A gear only counts as known when the total is non-zero and not smaller than
the raw index; the vendor then displays `index + 1` of the total
("Gear:%d/%d", plus `GEARS`, `FRONT GEAR`, `REAR GEAR`, `GEAR RATIO`,
`GEAR COMBO`, `SHIFTING BATT` data fields and the `COMP_ID_SHIFT_GEAR_CHART_*`
widgets). `main/shifting.c` does exactly this.

Batteries come from ANT+ common page 82 (`0x52`, decoder `FUN_421c26c8`):
`[2]` low nibble = number of batteries, high nibble = battery identifier
(the vendor keeps derailleurs and shifters apart by it), `[3..5]` cumulative
operating time (16 s units, 2 s when bit 7 of `[7]` is set), `[6]` fractional
voltage in 1/256 V, `[7]` bits 0-3 coarse voltage (`0x0F` = none), bits 4-6
status (1 new, 2 good, 3 ok, 4 low, 5 critical). The vendor's percentage
comes from a voltage curve of its own batteries; we show the voltage.

Not implemented here: the proprietary pages the vendor requests with common
page 70 (`FUN_421c0a0c` sends `46 FF FF FF FF 04 F5 01` for `0xF5`) - tooth
counts per gear (`%s:Get chainrings=0x%x,cassette=0x%x`, `Front/Rear Teech
Num Error`), shifter buttons (`0xF3`), shift modes and the Di2 pages of
`ant_di2.c`. A Di2 D-Fly pairs as device type 128 but its gears need those
private pages, so it shows no gear here.

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

## Calories and training zones (vendor algorithm)

Found through the double constant 0.6309 (`1c7c61325530e43f`) in the
**C606 Pro V1.723** image (`c606Pro_ota_0.elf`, the same code base): the
per-second ride data update `FUN_42263b60` (~2400 decompiled lines,
`RideDataProcess`-like; the literal pool at `0x42246778`) adds a calorie
increment to two accumulators (`+0xb0` total, `+0xb4` lap) and keeps a
rate in `+0xb8` (= total / elapsed s × 3600, the `kcal/h` field). The
accumulators are in **calories** (not kcal); the source is chosen in this
order (`+0xc0` = power meter present, `+0xa0` = HR present):

| source | cal per second | notes |
|---|---|---|
| power meter (uint16 `+0xcc`, W) | `W / 990 × 1000` | 1.01 kcal per kJ, i.e. ~24 % efficiency |
| heart rate (`+0xa8`, bpm) | Keytel et al. 2005: male (`+0x54 != 0`) `(-55.0969 + 0.6309·HR + 0.1988·kg + 0.2017·age) / 4.186` kcal/min, female `(-20.4022 + 0.4472·HR + 0.1263·kg + 0.074·age) / 4.186`; `× 1000 / 60`; negative → 0 | age = byte `+0x55` of the profile, weight = `_DAT_3c768940`; **the vendor adds the female weight term, the paper subtracts it** |
| neither | `MET × kg / 3600 × 1000` with speed (m/s × 3.6) → MET: 0 when standing, 4.2 < 16 km/h, 6.3 < 19.2, 8.4 < 22.4, 10.5 < 25.6, 12.6 < 30.6, 14.7 < 32, 16.8 above | multiples of 2.1 rather than the Compendium's 4.0/6.8/8.0/10/12/15.8 |

Our `main/health.c` uses the same three sources (with the paper's sign for
women). The zone UI strings (`HR Zones`, `Power Zones`, `%MAX HR`, `%LTHR`,
`Active Recovery` … `Anaerobic Capacity`) show the vendor keeps max HR,
LTHR and FTP in the profile synced from the app; the zone boundaries were
not chased — ours are Garmin's %max-HR (50/60/70/80/90), %LTHR
(68/85/90/95/100) and Coggan's %FTP (55/75/90/105/120/150) defaults.

---

# Magene C706 (second target)

Source: vendor firmware **C706 V1.729** (`ota_0-706.elf` in the same Ghidra
project, built May 30 2025), the full-flash backup
`magene_c706__my_stock_chinese.bin` and the device's own logs
(`LOG/<date>.log` on its eMMC). Addresses below refer to that image.
Verified on a real C706 on 2026-09-19 (first boot of `BOARD=c706`): 8 MB
octal PSRAM detected, panel and touch come up, nRF answers (fw 0.42.11,
reason 4), RTC/battery frames arrive, eMMC (8 GB `MV3608`) mounts, all
five keys report, GPS decodes at 115200 after the module has woken up
(the vendor parks it in RTC mode with `$PAIR650,0` and wakes it through the
nRF `E2 02 07 .. 01`, which is what our boot sends — the first NMEA came
~1 min after the flash), colours match the C606. This unit's `MAP/` holds
only `.etu` files (`mars_china_*_t2_v1741050274.101.etu`, `bg_china_*.etu`)
plus `map.key.101` (2048 bytes, probably their key) and `map.trans` — no
plain Mapsforge `.map`, so the map page stays hidden until one is copied in.

The two firmwares are the same code base (the C606 image even carries the
5-key handling and the `_CommonKeyD/E` callbacks); only the hardware
initialisation differs. Everything not listed here is identical to the C606:
nRF link (UART2 42/41 @ 115200, same frames), GPS on UART0 GPIO1/0 (Airoha —
`$PAIR…` traffic in the logs), eMMC on SDMMC 13/14/16/17/18/15, touch I2C0
SDA21/SCL12, GPIO43/44 driven high on the same NVS HW-variant flag.

## SoC / memory / flash

| Item | C606 | C706 |
|---|---|---|
| Flash | 16 MB DIO 80 MHz (header `4f`) | **32 MB** DIO 80 MHz (bootloader/app header byte 3 = `5f`) |
| PSRAM | 2 MB quad | **8 MB octal** (log: `AppDevInit free_heap_size = 8157220`; image has the `mspi_timing_*` PSRAM tuning code the C606 lacks) |
| Partitions | `ota_0` 0x20000/0x73A000, `ota_1` 0x760000 | `nvs` 0x9000/0x4000, `otadata` 0xd000, `phy_init` 0xf000, `coredump` 0x10000, **`ota_0` 0x20000/0xEA6000**, `ota_1` 0xED0000/0xEA6000 |
| Audio | — | ES8311 codec on I2C (0x18) + I2S, `mg_esp_adf` (bell, intercom/"talkback"); not used by this firmware |
| Wi-Fi | — | `WIFI/` + `WifiList.config` on the eMMC (vendor feature; not used) |

## Display

`MidLcdInit()` @ `0x42033f5c` (same file, lines 0x16d..0x22f):

* Panel: **AXS15231-family** (`mg_esp_lcd_new_panel_axs1523` @ `0x42034718`,
  `mg_panel_axs1523_*`), **320x480**, 16 bpp, `esp_lcd` **i80 bus, 8-bit wide**.
* `esp_lcd_i80_bus_config_t`: `dc = 40`, `wr = 3`, `clk_src = PLL_F160M`,
  `data_gpio_nums = {4, 38, 5, 48, 6, 47, 7, 11}`, `bus_width = 8`,
  `max_transfer_bytes = 0x4B000` (= 320·480·2, a full frame), `sram_trans_align = 4`.
  (`cs = 2`, RD `GPIO39` output high — as on the C606.)
* `esp_lcd_panel_io_i80_config_t`: `pclk = 20 MHz`, queue 10, cmd/param 8 bit,
  `dc_levels = {0,0,0,1}`, `flags = 0`.
* `esp_lcd_panel_dev_config_t`: `reset_gpio_num = -1`, `rgb_ele_order = BGR`,
  `bits_per_pixel = 16` — but the driver's `init` is a **no-op** (it only logs
  `mg_panel_axs1523_init`), so MADCTL/COLMOD are never sent: the panel boots
  configured on its own.
* `mg_panel_axs1523_reset` (`0x420344e8`): sends **`E2 02 09 00 00 02 00 00`**
  to the nRF (type 2, cmd 0x10 — the frame `A5 0C 6F F1 02 10 E2 02 09 00 00
  02 00 00 7A 3C` is even precomputed for the first call) and waits 150 ms.
  So `E2 02 09` is the display/touch module power: 0 off, 1 on, 2 reset pulse
  (the touch ESD recovery does 0 → 2 ms → 1 → 150 ms).
* After reset/init: `invert_color(false)` (INVOFF `0x20`), `set_gap(0,0)`.
* `mg_panel_axs1523_draw_bitmap` (`0x42034610`): CASET/RASET/RAMWR like the
  ST7789 driver, but it warns (`"444444"` log) when x/y start or end are not
  multiples of 4 → the port rounds every LVGL dirty area to 4 px.
* **Pixel byte order**: with an 8-bit bus the DMA sends the two bytes of a
  pixel in memory order. The vendor's LVGL 8 build has `LV_COLOR_16_SWAP = 1`
  (its `lv_palette` table is stored byte-swapped: `F2 06 E8 EC …`, whereas the
  C606 image has `06 F2 EC E8 …`) and bus `flags = 0`; we keep LVGL 9 in
  little-endian RGB565 and set `swap_color_bytes` on the panel IO instead.
* LVGL draw buffers: `mg_lvgl_port` (`0x42034b10`) allocates two
  `hres·vres/10` px = 30,720-byte DMA buffers (64-byte aligned); we use 48
  lines = the same size. Flush = `esp_lcd_panel_draw_bitmap`, tick 2 ms.

### Backlight (`MidLcdPwmInit` @ `0x42033e4c`)

LEDC low-speed, timer 0, 10-bit, 20 kHz, `LEDC_AUTO_CLK`; channel 0 on
**GPIO10** (C606: 45), duty 0 at init.

### Touch: AXS15231 integrated controller at 0x3B

`FUN_420343d4` probes **0x3B** first (`axs1523`), then 0x38 / 0x5A as on
the C606. Log on the real device: `New Dev Addr … 0x3b`, `axs_read_fw_version:19`.
The vendor does not use the usual single "B5 AB A5 5A 00 00 00 08" read;
each poll (`axs1523_read` @ `0x420358c0`, `FUN_420364e8`) is a handshake
over 11-byte command frames:

1. write `AB B5 5A A5 00 00 00 01 00 80 1F`, read 1 byte → must be `0x05`
   ("data ready"; `0x00`/`0x0A` = nothing, logged as *axs read point error*)
2. write `B5 AB A5 5A 00 00 00 0F 00 00 00` (length 15 big-endian at [6..7]), read **15 bytes**
3. write `AB B5 5A A5 00 01 00 00 00 80 1F 0A` (12 bytes, "consumed")

Record (seen on hardware, e.g. `00 01 80 78 01 5d 01 02 ff ff ff ff ff ff 56`):
`[1]` = number of points (1 or 2), `[2] >> 4` = event (0 press, 8 contact /
move, 4 lift-off), `x = ([2] & 0x0F) << 8 | [3]`, `y = ([4] & 0x0F) << 8 | [5]`
(raw 0..320 / 0..480, 1:1 on the panel), `[6]`/`[7]` frame counter, `[14]` =
**sum of bytes 0..13 mod 256**; `00 FF×13 F3` = idle/no touch. The status
byte is `0x05` only when a new record is pending — while idle that is once
every ~630 ms (the idle record), `0x00` in between; during a touch every poll.
Init: read the firmware version (`5A A5 AB B5 00 00 00 01 00 80 89` → 1 byte,
retried while ≤ 0x12) and enable the "ESD firmware"
(`B5 AB 5A A5 00 02 00 00 00 00 00 21 00`). After 56 consecutive polls without
a good record the vendor power-cycles the module via `E2 02 09` (0, then 1),
re-sends the ESD enable and redraws the screen (`axs_tp_esd_num reset`).
`main/touch.c` only logs at that point (`AXS_ESD_RECOVERY`): the power
cycle blanks the panel, and the counter stays around 20 in normal use.

## Keys: five (A..E = nRF index 0..4)

`KeyQueueReceive` (`0x42065c70`) stores 5 keys; the logs show all of
`Index=0..4`. Key types handed to `KmFunclHandler` are `0x2F + n` with
n = 0..4 click A..E, 5..9 long, 10..14 hold, 15..19 release
(`_CommonKeyELongCb` → `0x38`, `_CommonKeyEReleaseCb` → `0x42`). The two
`KeyMapInfo` scene tables (`0x3c822afc` = scene 1, `0x3c822b4c` = scene 0)
map a key type to a 1-based index into the `_KeyFunc_*` table at
`0x3c822e74` (0 = none):

| key | riding page (scene 1) | menus (scene 0) |
|---|---|---|
| A (0) | click **Lap**, hold **PowerOffPopUp** | click **ReturnBack**, hold PowerOffPopUp |
| B (1) | click **CarBell**, hold **StartTalkback** / release EndTalkback ("long press the button at the lower left corner … intercom") | — |
| C (2) | click **RidingCKeyPressFunc** (start/pause), hold RidingCKeyLongPressFunc | click **Trigger** (select), hold JumpAndStartRide |
| D (3) | click **SwitchPageHorRight** | click NextItem, hold FastNextItem |
| E (4) | click **SwitchPageHorLeft** | click PreItem, hold FastPreItem |

`board_c706.h` follows this: key 0 click = lap / hold = power-off, key 2 =
ride, key 3 / 4 = next / previous page, menus: 0 back, 2 select, 3 / 4
down / up. Key 1 is free (no bell or intercom here).

## GPS

`AppDevInit` (`0x4200bf98`) opens UART0 at **115200** (C606: 921600);
`GpsPthreadMachine` switches to the rate stored for the detected chip
(`MidCommBaudrateSwitch`) — `gps.c` probes 115200 first on this board and
the other rates after it. The logs show Airoha `$PAIR…` commands
(`$PAIR508`, `$PAIR496`, `$PAIR650`, `$PAIR470` EPO), so the same driver
applies.

---

# Magene C606 Pro (third target)

Source: vendor firmware **C606P V1.723** (`c606Pro_ota_0.elf`, built Sep 25
2025; raw image `~/devel/magene/esp32_image_parser/ota_0_out.bin`, 0x73A000
bytes = the C606 `ota_0` slot). Addresses refer to that image. **Not yet
tried on a C606 Pro.** The related Geoid CC700 Pro has the same pins (read
back live over JTAG) but the C606's panel init; it has its own profile
(`BOARD=cc700pro`, see the README, *Geoid CC700 Pro*). `BOARD=c606pro tools/build_podman.sh` →
`build-c606pro/c606pro_oss.bin`.

A hybrid of the other two: the C606's shell and peripherals on the C706's
display bus and memory.

| Item | C606 | C606 Pro |
|---|---|---|
| Panel | ST7789 240x320, **16-bit** i80 @ 15 MHz | ST7789 240x320, **8-bit** i80 @ 15 MHz, data `{4, 38, 5, 48, 6, 47, 7, 11}` (= C706), DC40 WR3 CS2 RD39, `max_transfer` 28800, two 28800-byte LVGL buffers (`MidLcdInit` @ `0x42033db4`) |
| Pixel order | as-is | byte-swapped (`LV_COLOR_16_SWAP = 1` palette signature `F2 06 E8 EC` at `0x3c40191c`) → `swap_color_bytes` |
| ST7789 init (`0x42034324`) | see above | after SWRESET + 20 ms and 10 ms: `36 00`, `3A 05`, `B2 0C 0C 00 33 33`, `B7 05`, `BB 23`, `C0 2C`, `C2 01`, `C3 15`, `C6 0F`, `D0 A7`, `D0 A4 A1`, `D6 A1`, `E0 F0 16 21 15 16 0F 46 54 55 20 1F 1F 3F 3B`, `E1 F0 0A 15 08 09 15 45 44 55 20 1F 1F 3F 3E`, `21`, `11`, `29`, `2C` (1 ms each); then `invert_color(true)`, `set_gap(0,0)`. `MidSendSleepToLCD` (`28`, `10`) right after init and `MidSendInitToLCD` (the sequence again) when the first screen is ready — we skip the sleep and init once |
| Backlight | GPIO45 | GPIO45 (`0x42033ca0`), same LEDC settings |
| Touch | FT6336 / CST328 on I2C0 21/12 | same (`0x420341f0`) |
| nRF / GPS | UART2 42/41, UART0 1/0 @ 921600 | same (`AppDevInit` @ `0x4200be64`: `MidCommInit(1, 115200)`, GPS 921600) |
| eMMC | SDMMC 13/14/16/17/18/15 | same (`0x4200d4ac`) |
| Flash / PSRAM | 16 MB DIO 80 MHz (`4f`), 2 MB quad | 16 MB DIO 80 MHz (`4f`), **octal** PSRAM (`octal_psram` driver + `mspi_timing` tuning in the image; size unknown until it boots) |
| Keys | 3 | 3 — the `KeyMapInfo` scene tables (`0x3c5b7340` riding, `0x3c5b7390` menus) have no D/E entries: riding A = Lap, B = SwitchPageHorRight, C = start/pause; menus A = back, A hold = power-off. `board_c606pro.h` keeps the C606 build's roles |
| Draw window | any | any (no alignment check in `mg_panel_st7789_draw_bitmap` @ `0x420345fc`) |

(Correction to the C706 key notes above: in the riding scene the vendor's
A hold is `BreathPlate`; `PowerOffPopUp` is A hold in scene 0.)
