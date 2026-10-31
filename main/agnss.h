#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Called after fresh SNTP and with Wi-Fi connected. Downloads a vendor
 * archive over HTTPS, validates it, then sends the current GPS Host EPO set.
 * All receiver writes are RAM assistance commands, never flash updates. */
esp_err_t agnss_update(unsigned *accepted, uint32_t *expires);
/* Developer-console test / offline use with a trustworthy current UTC. */
esp_err_t agnss_apply_cached(uint32_t utc, unsigned *accepted, uint32_t *expires);
