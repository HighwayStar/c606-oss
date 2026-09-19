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

#define CFG_MAX_SENSORS 8
typedef struct {
    uint16_t dev_num;
    uint8_t dev_type;                   /* ANT+ device type */
    uint8_t trans_type;
} cfg_sensor_t;

typedef struct {
    uint16_t magic;
    uint8_t version;
    int16_t tz_min;                     /* local time = UTC + tz_min */
    page_cfg_t page[CFG_PAGES];
    uint8_t theme;                      /* theme_id_t (added in version 2) */
    uint16_t lap_len_m;                 /* auto lap every N metres, 0 = off (version 3) */
    uint8_t backlight;                  /* percent, 10..100 (version 4) */
    uint8_t auto_pause;                 /* 0 / 1 (version 5) */
    /* version 6 */
    uint16_t wheel_mm;                  /* wheel circumference for the speed sensor */
    uint8_t nsensors;
    uint8_t sensors_imported;           /* vendor sensor_list.json copied once */
    cfg_sensor_t sensors[CFG_MAX_SENSORS];
    page_cfg_t map_page;                /* map page: enabled, layout Map/M1/M2, strip fields (version 7) */
    uint32_t map_layers;                /* bit per mapview layer group, 1 = drawn (version 8) */
    char route[40];                     /* GPX file in /sdcard/c606oss/routes shown on the map, "" = none (version 9) */
    uint8_t theme_auto;                 /* light theme by day, dark after sunset (version 10) */
    uint8_t route_reverse;              /* ride the route from its end to its start (version 11) */
    /* rider profile (version 12), see health.h */
    uint8_t weight_kg;
    uint8_t height_cm;
    uint16_t birth_year;
    uint8_t sex;                        /* 0 = male, 1 = female */
    uint8_t max_hr;                     /* bpm, 0 = estimate from the age */
    uint8_t lthr;                       /* lactate threshold HR, 0 = estimate from max HR */
    uint8_t hr_zone_mode;               /* hr_zone_mode_t: zones as % of max HR or of LTHR */
    uint16_t ftp_w;                     /* functional threshold power, 0 = unknown (no power zones) */
    uint16_t bike_kg10;                 /* bike weight in 0.1 kg, for the FIT bike_profile (version 13) */
} app_cfg_t;

/* Page configs by index; CFG_PAGES = the map page. */
static inline page_cfg_t *config_page(app_cfg_t *c, int idx)
{
    return idx < CFG_PAGES ? &c->page[idx] : &c->map_page;
}

#define CFG_WHEEL_MIN_MM 1000
#define CFG_WHEEL_MAX_MM 3000

/* Sensor list helpers; both save the config. */
bool config_sensor_add(uint8_t dev_type, uint16_t dev_num, uint8_t trans_type);
void config_sensor_remove(int idx);

#define CFG_WEIGHT_MIN_KG 30
#define CFG_WEIGHT_MAX_KG 200
#define CFG_HEIGHT_MIN_CM 100
#define CFG_HEIGHT_MAX_CM 230
#define CFG_BIRTH_MIN     1920
#define CFG_BIRTH_MAX     2020
#define CFG_HR_MIN        100      /* max HR / LTHR when set by hand; 0 = auto */
#define CFG_HR_MAX        230
#define CFG_FTP_MAX_W     600
#define CFG_BIKE_MAX_KG10 500

#define CFG_LAP_STEP_M 500
#define CFG_LAP_MAX_M  10000

app_cfg_t *config_get(void);
void config_defaults(app_cfg_t *c);
void config_load(void);                 /* NVS -> config_get(); defaults if absent/invalid */
esp_err_t config_save(void);
