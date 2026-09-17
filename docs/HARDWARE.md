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

### Touch (C606 Pro / newer HW)

`FUN_42031b14`: I2C touch controller probed at address **0x38** (FocalTech
FT6x36 style) then **0x5A**. Registered with LVGL as a pointer input device
("touch_driver_read"). I2C pins not yet extracted.

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

Sensor stream `cmd 0x10, F1 <sub>`, ~5 Hz, starts some time after boot
(not seen in the first 45 s; possibly motion-triggered). Interpretation of
the values is a hypothesis from the vendor's math and plausibility:

| sub | payload[2..7] | reading |
|---|---|---|
| 01 | 3 x int16 | accelerometer raw, ~4096 LSB/g (`3C 06 F0 04 1A 0E` -> magnitude ≈ 1.01 g at rest) |
| 02 | 3 x int16 | gyro raw (`ED FF F7 FF 1B 00` = -19, -9, 27 at rest) |
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

* **SD card**: SDMMC 4-bit, CLK 13, CMD 14, D0 16, D1 17, D2 18, D3 15 (`MidVFSMount`), mounted at `/sdcard`.
* **NVS**: standard `nvs` partition; vendor config blob `Res1Page11` (byte 3 = HW variant).
* GPIO43/44 (default UART0 pins) are driven high as outputs on HW variant 2 before LCD init.
