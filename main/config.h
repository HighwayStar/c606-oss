#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "layouts.h"

/* User configuration: the data pages (layout + field per cell) and a few
 * settings. Persisted as one blob in the NVS partition, in our own
 * namespace next to the vendor's data (which is never touched). */

#define CFG_PAGES 5

typedef struct {
    uint8_t enabled;
    uint8_t layout;                     /* index into layouts.c */
    uint8_t field[LAYOUT_MAX_CELLS];    /* field_id_t per cell */
} page_cfg_t;

typedef struct {
    uint16_t magic;
    uint8_t version;
    int16_t tz_min;                     /* local time = UTC + tz_min */
    page_cfg_t page[CFG_PAGES];
    uint8_t theme;                      /* theme_id_t (added in version 2) */
    uint16_t lap_len_m;                 /* auto lap every N metres, 0 = off (version 3) */
    uint8_t backlight;                  /* percent, 10..100 (version 4) */
} app_cfg_t;

#define CFG_LAP_STEP_M 500
#define CFG_LAP_MAX_M  10000

app_cfg_t *config_get(void);
void config_defaults(app_cfg_t *c);
void config_load(void);                 /* NVS -> config_get(); defaults if absent/invalid */
esp_err_t config_save(void);
