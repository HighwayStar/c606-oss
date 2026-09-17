#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Expose the eMMC to the USB host as a mass-storage device.
 * One-way: the USB PHY is taken from USB-Serial-JTAG (console/flashing die
 * until reboot). Stops recording and unmounts /sdcard from the app first. */
esp_err_t usb_msc_enter(void);
bool usb_msc_active(void);

/* Restore the USB PHY to USB-Serial-JTAG. Called at boot (safety net after a
 * crash in MSC mode) and before leaving MSC mode. */
void usb_phy_route_to_serial_jtag(void);

/* Uninstall TinyUSB, restore the PHY and reboot. */
void usb_msc_leave_and_restart(void);
