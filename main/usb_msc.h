#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Expose the eMMC to the USB host as a mass-storage device.
 * One-way: the USB PHY is taken from USB-Serial-JTAG (console/flashing die
 * until reboot). Stops recording and unmounts /sdcard from the app first. */
esp_err_t usb_msc_enter(void);
bool usb_msc_active(void);
