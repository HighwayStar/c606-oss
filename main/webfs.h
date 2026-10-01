#pragma once
#include "esp_err.h"

/* File manager web server over the eMMC (port 80), started by wifi_ap.c.
 *
 *   GET    /                       the page (main/web/index.html)
 *   GET    /api/ls?d=/dir          JSON: free / total bytes, entries
 *   POST   /api/mkdir?d=/dir
 *   POST   /api/mv?from=/a&to=/b
 *   GET    /fs/<path>[?dl=1]       download (dl: as an attachment)
 *   PUT    /fs/<path>              upload (raw body; written to <path>.part,
 *                                  renamed over <path> when complete)
 *   DELETE /fs/<path>              file, or an empty directory
 *
 * Paths are relative to the card root, percent-encoded; ".." is refused. */

esp_err_t webfs_start(void);
void webfs_stop(void);
