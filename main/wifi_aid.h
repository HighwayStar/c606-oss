#pragma once
#include <stddef.h>
#include "esp_err.h"

/* One-shot Wi-Fi station connection -> SNTP -> GPS UTC assistance.
 * Radio is stopped afterwards; no network activity on boot. */
esp_err_t wifi_aid_start(const char *ssid, const char *password);
void wifi_aid_status(char *out, size_t size);
void wifi_aid_load(char ssid[33], char password[65]);
