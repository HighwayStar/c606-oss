#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Developer console on the USB-Serial-JTAG port (the normal log console).
 * Line commands:
 *   key <idx> <evt>   inject a button event (evt 1 click, 4 hold, 5 release)
 *   tap <x> <y>       inject a touch tap at screen coordinates
 *   shot              dump the screen as hex rows ("SHOT 240 320" .. "SHOT_END")
 *   spd <kmh>         feed one speed sample to the auto-pause logic
 *   ls [dir]          list a directory (default /sdcard/c606oss): "name size" .. "LS_END"
 *   get <path>        dump a file as hex rows ("FILE <size>", 'F'.., "FILE_END")
 *   mv <old> <new>    rename a file
 *   pos <lat> <lon>   centre the map page on this position instead of the GPS ("pos" alone: back to GPS)
 *   zoom <z>          map zoom level (10..17)
 *   nmea <sentence>   feed a NMEA sentence to the GPS parser (the receiver is muted until "nmea off")
 *   heap              print free heap
 * tools/devcon.py drives it from the host. */

typedef void (*devcon_key_cb_t)(uint8_t key, uint8_t evt);
esp_err_t devcon_init(devcon_key_cb_t key_cb);

/* Called by the display flush: keeps a copy of the frame for `shot`. */
void devcon_mirror(int x1, int y1, int x2, int y2, const void *px);
