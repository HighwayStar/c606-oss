#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Wi-Fi access point + the file transfer web server (webfs.c).
 *
 * SSID "<board>-XXXX" (last MAC bytes), WPA2 with a random password made
 * on the first start and kept in NVS (namespace c606oss, key "wifipw"),
 * the device is 192.168.4.1. The AP turns itself off after
 * WIFI_AP_IDLE_MIN minutes without a connected client or HTTP request.
 * Start / stop may be called from any task (they serialise on a mutex). */

#define WIFI_AP_IDLE_MIN 10
#define WIFI_AP_URL "http://192.168.4.1/"

esp_err_t wifi_ap_start(void);
void wifi_ap_stop(void);
bool wifi_ap_active(void);
const char *wifi_ap_ssid(void);       /* valid after the first start */
const char *wifi_ap_password(void);
int wifi_ap_clients(void);            /* stations connected now */
/* "WIFI:T:WPA;S:<ssid>;P:<pw>;;" for a join QR code */
void wifi_ap_qr_text(char *buf, int n);
/* an HTTP request was served: postpones the idle switch-off */
void wifi_ap_touch(void);
