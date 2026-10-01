# User guide

How the firmware behaves on the device. For installing it see
[FLASHING.md](FLASHING.md); for building and the developer tools see
[DEVELOPMENT.md](DEVELOPMENT.md).

## Keys and touch

| | C606 / C606 Pro / CC700 Pro | C706 |
|---|---|---|
| Next page | key 0 | key 3 |
| Previous page | — | key 4 |
| Lap | key 1 | key 0 |
| Start / pause / resume | key 2 | key 2 |
| End ride | hold key 2 | hold key 2 |
| Power off | hold key 0 | hold key 0 |
| Menus | 2 = up, 1 = down, 0 = select | 4 / 3 = up / down, 2 = select, 0 = back |

On the touch screen, **swipe left / right** for the next / previous page and
tap the **gear icon** in a page header for the settings. Idle, the pages are
idle ↔ status ↔ map; during a ride, the enabled data pages and the map.

## Idle screen and status page

Idle screen: clock (nRF RTC + time zone), GPS fix / satellites, paired
sensor values, today's sunrise / sunset and a *START RIDE* button. Key 0 or a swipe switches to the
status page (battery arc with mV, temperature and pressure, `nRF ok reason 4
fw 0.2.19`, GPS and eMMC state, three key boxes, event log, heap footer) and
back. A key box flashes green on a click and stays red while long-pressed.

## Riding

Key 2 (or *START RIDE*) starts a ride: statistics are reset, a FIT file is
opened in `/sdcard/c606oss/<local date-time>.fit` (see [DEVELOPMENT.md](DEVELOPMENT.md#fit-files) for what goes into it) and the
data pages appear ("Page 1" … "Page 5", only the enabled ones; key 0 or
a swipe cycles). The header shows `▶ h:mm:ss` (session time). Key 2 pauses (`‖`,
recording and statistics stand still, the time excludes pauses) and resumes.
Hold key 2 for the *End ride?* dialog: key 2 / *End* closes the track file
and shows the ride summary (time, distance, avg/max speed, avg/max HR, avg
cadence and power, calories, max altitude, laps); any key or *Done* returns to the
idle screen, anything else in the dialog cancels. The file only opens once
the clock is known (nRF RTC, else the GPS date — after 20 s without either
it records anyway, timestamped from 2000-01-01). With *Auto pause*
on, standing still (below 1.5 km/h for 3 s, wheel sensor or GPS) pauses the
ride by itself (`‖ … auto`) and moving again (> 3 km/h) resumes it; a manual
pause is never auto-resumed.

## Data pages and fields

Up to 5 user-configurable data pages, Bryton/Magene style: each page has
one of 18 cell layouts ("fences" 1, 2, 3A … 12) and every cell shows one
data field (Speed, Max Speed, Avg HR, Time of Day, …). Configured on the
device in the settings menu (gear icon), saved in NVS.

Data page cells show a field's name and value (font sized to the cell;
grey `--` when no fresh reading arrived within a few seconds). Fields are
the current / min / max / avg of every measured parameter, distance and
laps (Distance, Laps, Lap Dist, Lap Time, Lap Speed, PreLap Time, PreLap
Dist), *Route Left* (km along the loaded GPX route from the nearest point
of the track to its end, see *Route* below; `--` without a route or a
fix), time of day, session time, battery %, satellites, heading
(compass point from the GPS course while moving), sunrise / sunset
(local HH:MM for the current local date at the last GPS position, `24h` /
`none` on polar days; `--:--` until both a position and the clock are
known — the position is saved in NVS whenever it moves ~10 km from the
saved one, so it survives power cycles) and *Sunset in* (h:mm left until
today's sunset, `--` once the sun is down). The *Health* category has
*Calories* (kcal so far: from the power meter while it is live, else
from the HR strap, else from the speed — `main/health.c`), *Cal Rate*
(kcal/h over the session, after the first minute), *HR Zone* (`Z0`–`Z5`
of the live HR), *%Max HR*, *%LTHR*, *Zone Time* (how long the HR has
been in its current zone), *Power Zone* (`Z0`–`Z7`, needs an FTP),
*%FTP* and *Power/kg*. The *Gears* category has *Gear* (the rear gear as
`3/13` - which gear of how many), *Front Gear* (`1/2`), *Gear Combo*
(`2x11`, the front and rear positions; just the rear gear on a 1x
drivetrain) and *Shift Batt* (a Di2's battery percentage, else the
group's lowest battery voltage, else its ANT+ status word); all show `--`
without a shifting sensor or 10 s after its last page. Distance comes
from the ANT+ wheel sensor when it is live (revs × `ANT_WHEEL_CIRC_M`),
otherwise from consecutive GPS fixes (moving faster than 2 km/h); a lap
ends automatically every *Lap length*; key 1 ends the current lap by hand
(a "LAP n" toast confirms it). The average is time-weighted, so it
does not depend on how often a sensor reports; cadence and HR ignore zero
samples. *Reset statistics* in the menu restarts the session by hand.

## Settings menu

Tap the gear icon in a page header. Touch or keys work (C606: key 2 = up,
key 1 = down, key 0 = select; selecting the back arrow goes back):

* *Pages* → *Page n* → *Enable*, *Layout* (preview of the page with an
  up/down selector, tick applies), *Fields* (preview of the page: tap a cell,
  or move the yellow frame with the keys and press key 0, then pick a
  category and a field). The last entry, *Map*, configures the map page
  the same way with the layouts *Map* / *M1* / *M2*.
* *Profile* → *Weight*, *Height*, *Year of birth* (+/− screens), *Sex*
  (tap to toggle), *Max HR* and *LTHR* (`auto (184)` = estimated as
  220 − age and 89 % of max HR; + from auto starts at the estimate),
  *FTP* (5 W steps, off = no power zones), *HR zones by* (%Max HR /
  %LTHR), *Bike weight* (0.1 kg steps, only written to the FIT
  `bike_profile`) and *Health*: age, BMI with its class, BMR (Mifflin-St Jeor),
  the HR zone table in bpm and the power zone table in W. The defaults
  (75 kg, 175 cm, 1990, male) are a placeholder — set yours, the calorie
  count depends on them.
* *Sensors* → *Wheel* (circumference in mm, +/− screen, hold to run) and
  the known ANT+ sensors with their state (`ok` / `searching` / `--`); tap
  one to *Forget* it, *Add sensor* runs a 30 s ANT scan and lists what it
  finds with RSSI — tap a result to pair and connect it.
* *Lap length*: +/− screen, 0.5 km steps, 0 = off (touch the buttons or
  key 2 / key 1, hold to run, key 0 goes back; the value is saved when
  leaving the screen).
* *Auto pause* on/off.
* *Backlight*: +/− screen, 10 % steps, applied live and remembered.
* *Time zone*: same +/− screen in 30 min steps (UTC-12 … UTC+14) for the
  time of day, which comes from the nRF's RTC (UTC), GPS as fallback.
* *Theme*: dark (default) or light, applied immediately (`main/theme.c`:
  shared LVGL styles; status colours are darkened on the light background).
* *Auto theme*: light between sunrise and sunset, dark otherwise, checked
  twice a second from the sun times above (so it also follows the clock
  and time zone). Applied at once when switched on; picking a theme by
  hand switches it off again.
* *Map layers*: a toggle per road class (motorway/trunk, primary, …,
  track, cycleway), water, coastline and "other" — switched-off layers
  are skipped before their coordinates are even decoded, so a
  roads-only map renders a little faster, never slower.
* *Route*: one of the `.gpx` files in `/sdcard/c606oss/routes/` (the
  loaded one is ticked, "rev." when reversed; the settings row shows its length) or
  the first row — *None*, which becomes *Unload route* while a route is
  loaded — to take the track off the map. Picking a file opens a preview:
  the track outline with start (green) / end (red) markers, length and
  point count, total climb / descent with the elevation range when the
  points carry `<ele>` (a 5 m hysteresis keeps GPS noise from adding up;
  "No elevation data" otherwise), a *Reverse* toggle (ride the track from
  its end — swaps the markers and the climb / descent) and *Use this
  route*. `main/route.c` scans the file for `<trkpt>` / `<rtept>` lat/lon
  attributes and their `<ele>` child, so GPX 1.0/1.1 tracks and routes
  from any tool work; points are kept as Web-Mercator pixels in PSRAM
  (long tracks are thinned to 16k points, the preview to 512), the
  choice and the direction persist in the config. Every GPS fix is
  matched to the nearest point of the loaded track for the *Route Left*
  field; the match prefers the stretch it was on last time, so an
  out-and-back track does not flip to the other leg where both run along
  the same road.
* *History*: the recorded rides, newest first (the date and time from
  the file name, so the list is instant; the ride being recorded is not
  listed; rides recorded by the vendor firmware — `FITS/FIT/<unix
  time>.fit` on the card — are included and tagged *Magene*). A ride
  opens with its track outline ("No GPS track" for an indoor ride) and a
  3 × 4 summary: timer time, distance, avg / max speed, avg / max HR,
  cadence, power, calories, climb / descent and laps. The values come
  from the file's `session` message; a file without one (power lost
  mid-ride, or the vendor's files, which end with a single `lap` instead)
  gets them from that lap or, failing that, from the records
  themselves. *Use as route* makes the ride the map's route (a `.fit`
  name in the *Route* setting, listed on top of the GPX files there) and
  *Delete ride* removes the file after a confirmation.
* *Reset statistics*.
* *System* → *USB storage*, *Wi-Fi transfer*, *Power off*, *Reset
  settings* (with a confirmation: everything back to the firmware
  defaults), *About*.

## USB storage and power off

Settings → System → USB storage: the eMMC appears on the host as a 3.7 GB
USB disk (`303a:4002 c606-oss C606 eMMC`, auto-mounted by most desktops).
The S3 has a single USB PHY, so the console and esptool auto-reset are gone
while in this mode — eject the disk, then tap *Reboot* or hold key 1.
Hold key 0: "Power off?" popup — key 0 again (or tap Off) powers off via
the nRF, any other key (or Cancel, or 8 s) dismisses it. Press key 0 to
power on again.

## Wi-Fi transfer

Settings → System → Wi-Fi transfer → *Access point* turns on a Wi-Fi
network (`C606-XXXX`, WPA2; the password is made up on the first use and
then stays the same) with a file manager page at `http://192.168.4.1/`.
The screen shows a QR code: scan it with a phone camera to join the
network; once a client is connected the code switches to the page's
address (tap the code to flip between the two). SSID, password and
address are printed below it for a computer.

The page browses the whole card: download (tap a name), upload (button
or drag and drop, several files at once, with progress), new folder,
rename / move (a name, or a path starting with `/`), delete (folders
only when empty). Uploads go to `<name>.part` and replace the target
only when complete, so an interrupted upload leaves the old file alone.
Files the device has open (the ride being recorded, the maps in `MAP/`)
can't be replaced, renamed or deleted — the page says "in use"; new maps
are picked up after a restart. Unlike USB storage the device keeps
running (riding, recording) while the network is up.

The network stays up after leaving the menu and switches itself off
after 10 minutes without a connected client; turning on USB storage or
powering off also stops it.

## Sensors

ANT+ sensors through the nRF: HR, speed, cadence, power decoded. The paired
list lives in our own config (the vendor's `CONFIG/sensor_list.json` is
imported once on first run); Settings → Sensors lists them with their live
state, forgets them, adds new ones through an ANT scan, and sets the wheel
circumference.

**Electronic shifting** (`main/shifting.c`): an ANT+ shifting sensor
(device type 34 - SRAM AXS / eTap, Magene QED, Shimano in ANT+ mode) or a
Shimano **Di2** D-Fly (device type 128, its own pages) is paired like any
other sensor and gives the *Gear* (`3/13`, current of total), *Front
Gear*, *Gear Combo* and *Shift Batt* data fields; a Di2 is asked for its
gear counts the way the vendor does it (the private page
`80 08 FF 00 FF FF FF FF` every 10 s until it answers), and until it
answers the total shows as `--`; every shift is written to the FIT file as
a standard `front_gear_change` / `rear_gear_change` event and the
drivetrain's gear counts go into `bike_profile`, so Garmin Connect and
friends show the gear timeline.

## Profile, calories and training zones

`main/health.c`: weight, height, year of birth, sex, max HR, LTHR and FTP
in Settings → Profile; calories are integrated every second of the ride
from the best source — power meter (1.01 kcal/kJ), else heart rate (Keytel
et al. 2005), else speed (MET table) — the same three-tier model as the
vendor firmware (see [HARDWARE.md](HARDWARE.md)); HR zones 1–5 as % of max
HR or of LTHR, power zones 1–7 as % of FTP; a *Health* screen with BMI, BMR
and the zone tables; the FIT file carries `user_profile`, `zones_target`,
`total_calories` and the time-in-zone arrays per lap and session.

## Sunrise / sunset

`main/sun.c`, NOAA equations, for the day and the last GPS position —
*Sunrise*, *Sunset* and *Sunset in* (h:mm countdown) data fields for any
cell, a line on the idle screen, and an *Auto theme* setting that uses the
light theme by day and the dark one after sunset. The position is
remembered in NVS, so the times are there right after power-on, before the
receiver has a fix.

## Maps

The map page reads the vendor's vector maps (`MAP/*.map` on the eMMC —
plain Mapsforge binary files, see [HARDWARE.md](HARDWARE.md)) and draws the
roads around the GPS position; +/− zoom buttons (zoom 10–17), scale bar,
light and dark palettes following the theme, rendered in its own task in
well under 100 ms per view. Configured like the data pages (Settings →
Pages → Map): enable, layout *Map* (map only), *M1* / *M2* (one or two data
fields in a strip under the map, Speed + Heading by default), fields per
cell. While riding, the path ridden so far is drawn on the map as a blue
line (`main/trail.c`: the GPS fixes of the ride, kept as Mercator pixels in
PSRAM, thinned when they exceed 16k points; on top of the route, so the
part of it already covered turns blue; cleared when the next ride starts).

The map page lists `/sdcard/MAP/*.map` at boot (the vendor's
Mapsforge files; the encrypted `.etu` files are ignored). Without a `.map`
file the page is not in the page ring. Vendor maps for other regions are
produced with the mapsforge map-writer, so any Mapsforge v3-v5 map works,
including the official ones from `download.mapsforge.org` (checked with
`v5/europe/germany/berlin.map`; copy them to `MAP/` in USB storage mode;
files must stay below 2 GiB, tile offsets go through a 32-bit `fseek`) —
`tools/mapdump/` builds the same reader on the host (`build.sh`, then
`mapdump file.map lat lon zoom out.ppm`) for checking a file without the
device. File names containing "china" are treated as GCJ-02 (the fix is
shifted to match); the Siberia map is plain WGS-84 despite its "mars_"
prefix.

## Routes and ride history

**GPX routes**: copy `.gpx` tracks/routes to `c606oss/routes/` on the USB
disk, pick one in Settings → Route — a preview shows the track outline,
length, climb / descent (from `<ele>`, when present) and a *Reverse* toggle
before it is loaded — and it is drawn on the map (magenta, green start /
red end) and a *Route Left* data field counts down the track distance to
its end; the map page works with a route even without a map file.

**Ride history**: Settings → History lists the recorded rides (ours in
`c606oss/`, and the vendor firmware's in `FITS/FIT/`, tagged *Magene*)
newest first; a ride opens with its track outline and the summary — time,
distance, avg / max speed, avg / max HR, cadence, power, calories, climb /
descent, laps (`main/fitread.c`, a small tolerant FIT reader) — and can be
*used as route*: it becomes the map's route like a GPX file, or deleted.

## Troubleshooting

**Temperature / pressure show `--.-` after flashing?** The nRF only streams
its IMU/baro data after the power key has been *held* (its power-on gesture).
An esptool or software reset skips that, so hold key 0 once and cancel the
popup; a normal button power-on doesn't need it.

**If the ESP32 is ever unreachable** (no USB device, screen frozen): hold all
three buttons for a few seconds — the nRF performs a hardware reset / power
cycle of the ESP32 regardless of what the firmware is doing.

If the screen stays dark: check the backlight (GPIO45) first — the bars are
drawn before it is enabled, so a dark-but-flickering panel means the i80 bus
works. If colours are swapped (red <-> blue) build with
`-DLCD_MADCTL=0x08` (BGR bit) in `main/CMakeLists.txt`.
