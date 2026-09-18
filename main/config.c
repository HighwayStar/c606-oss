/*
 * Page / settings persistence in NVS (namespace "c606oss", key "cfg").
 * The vendor's NVS entries live in the same partition; we only ever read
 * and write our own namespace and never erase the partition.
 */
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "config.h"
#include "fields.h"

static const char *TAG = "config";
#define CFG_MAGIC   0xC606
#define CFG_VERSION 8
/* keep k_len_by_version in config_load() in sync when appending fields */
_Static_assert(sizeof(app_cfg_t) == 136, "app_cfg_t layout changed: add its size to k_len_by_version");
#define NVS_NS      "c606oss"
#define NVS_KEY     "cfg"

static app_cfg_t s_cfg;
static bool s_nvs_ok;

app_cfg_t *config_get(void) { return &s_cfg; }

static void set_page(page_cfg_t *p, bool enabled, const char *layout, const field_id_t *fields, int n)
{
    memset(p, 0, sizeof *p);
    p->enabled = enabled;
    p->layout = layout_index(layout);
    for (int i = 0; i < n && i < LAYOUT_MAX_CELLS; i++) p->field[i] = fields[i];
}

void config_defaults(app_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = CFG_MAGIC;
    c->version = CFG_VERSION;
    c->tz_min = 0;
    c->lap_len_m = 1000;
    c->backlight = 70;
    c->auto_pause = 1;
    c->wheel_mm = 2105;

    static const field_id_t p1[] = {
        FIELD_TIME_OF_DAY, FIELD_STAT(STAT_SPEED, AGG_CUR), FIELD_STAT(STAT_SPEED, AGG_AVG),
        FIELD_STAT(STAT_HR, AGG_CUR), FIELD_STAT(STAT_CADENCE, AGG_CUR),
        FIELD_DISTANCE, FIELD_SESSION_TIME,
    };
    static const field_id_t p2[] = {
        FIELD_TIME_OF_DAY, FIELD_STAT(STAT_ALTITUDE, AGG_CUR),
        FIELD_STAT(STAT_TEMP, AGG_CUR), FIELD_STAT(STAT_PRESSURE, AGG_CUR),
        FIELD_STAT(STAT_SPEED, AGG_MAX), FIELD_STAT(STAT_HR, AGG_MAX),
    };
    static const field_id_t p3[] = {
        FIELD_STAT(STAT_SPEED, AGG_CUR), FIELD_STAT(STAT_SPEED, AGG_MAX),
        FIELD_STAT(STAT_SPEED, AGG_AVG), FIELD_STAT(STAT_HR, AGG_CUR),
        FIELD_STAT(STAT_HR, AGG_MAX), FIELD_STAT(STAT_HR, AGG_AVG),
        FIELD_STAT(STAT_CADENCE, AGG_CUR), FIELD_STAT(STAT_CADENCE, AGG_AVG),
        FIELD_STAT(STAT_POWER, AGG_CUR), FIELD_STAT(STAT_POWER, AGG_AVG),
        FIELD_STAT(STAT_ANT_SPEED, AGG_CUR), FIELD_SATS,
    };
    static const field_id_t p4[] = {
        FIELD_LAPS, FIELD_LAP_DIST, FIELD_LAP_TIME, FIELD_LAP_SPEED, FIELD_PRELAP_TIME, FIELD_DISTANCE,
    };
    set_page(&c->page[0], true, "7A", p1, sizeof p1);
    set_page(&c->page[1], true, "6C", p2, sizeof p2);
    set_page(&c->page[2], true, "12", p3, sizeof p3);
    set_page(&c->page[3], false, "6A", p4, sizeof p4);
    set_page(&c->page[4], false, "1", p1, 1);
    static const field_id_t pm[] = { FIELD_STAT(STAT_SPEED, AGG_CUR), FIELD_HEADING };
    set_page(&c->map_page, true, "M2", pm, sizeof pm);
    c->map_layers = 0xFFFFFFFF;
}

static void map_page_defaults(app_cfg_t *c)
{
    app_cfg_t d;
    config_defaults(&d);
    c->map_page = d.map_page;
}

static bool valid(const app_cfg_t *c)
{
    if (c->magic != CFG_MAGIC || c->version != CFG_VERSION) return false;
    if (c->theme > 1) return false;
    if (c->lap_len_m > CFG_LAP_MAX_M) return false;
    if (c->backlight < 10 || c->backlight > 100) return false;
    if (c->auto_pause > 1) return false;
    if (c->wheel_mm < CFG_WHEEL_MIN_MM || c->wheel_mm > CFG_WHEEL_MAX_MM) return false;
    if (c->nsensors > CFG_MAX_SENSORS) return false;
    for (int p = 0; p <= CFG_PAGES; p++) {
        const page_cfg_t *pg = config_page((app_cfg_t *)c, p);
        if (pg->layout >= layout_count()) return false;
        if (!!layout_get(pg->layout)->map != (p == CFG_PAGES)) return false;
        for (int i = 0; i < LAYOUT_MAX_CELLS; i++) {
            if (pg->field[i] >= FIELD_COUNT) return false;
        }
    }
    return c->tz_min >= -12 * 60 && c->tz_min <= 14 * 60;
}

void config_load(void)
{
    config_defaults(&s_cfg);
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* never erase: the partition holds the vendor's settings and the
         * phy calibration */
        ESP_LOGW(TAG, "nvs_flash_init: %s - settings will not persist", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return;
    }
    app_cfg_t tmp;
    memset(&tmp, 0, sizeof tmp);
    size_t len = sizeof tmp;
    err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
    nvs_close(h);
    /* Older blobs are prefixes of the current struct (fields are only ever
     * appended) but their length includes the tail padding of that version,
     * so the sizes are listed explicitly. The struct was zeroed before the
     * read; fill in the defaults of the fields the blob does not have. */
    static const size_t k_len_by_version[] = { 0, 76, 78, 80, 82, 82, 118, 132, 136 };
    if (err == ESP_OK && tmp.magic == CFG_MAGIC && tmp.version >= 1 && tmp.version < CFG_VERSION
        && len == k_len_by_version[tmp.version]) {
        if (tmp.version < 2) tmp.theme = 0;
        if (tmp.version < 3) tmp.lap_len_m = 1000;
        if (tmp.version < 4) tmp.backlight = 70;
        if (tmp.version < 5) tmp.auto_pause = 1;
        if (tmp.version < 6) { tmp.wheel_mm = 2105; tmp.nsensors = 0; tmp.sensors_imported = 0; }
        if (tmp.version < 7) map_page_defaults(&tmp);
        if (tmp.version < 8) tmp.map_layers = 0xFFFFFFFF;
        ESP_LOGI(TAG, "config upgraded from version %u", tmp.version);
        tmp.version = CFG_VERSION;
        len = sizeof tmp;
    }
    if (err == ESP_OK && len == sizeof tmp && valid(&tmp)) {
        s_cfg = tmp;
        ESP_LOGI(TAG, "config loaded");
    } else {
        ESP_LOGW(TAG, "saved config unusable (%s, %u bytes), using defaults", esp_err_to_name(err), (unsigned)len);
    }
}

esp_err_t config_save(void)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof s_cfg);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}

bool config_sensor_add(uint8_t dev_type, uint16_t dev_num, uint8_t trans_type)
{
    for (int i = 0; i < s_cfg.nsensors; i++) {
        if (s_cfg.sensors[i].dev_type == dev_type && s_cfg.sensors[i].dev_num == dev_num) return true;
    }
    if (s_cfg.nsensors >= CFG_MAX_SENSORS) return false;
    cfg_sensor_t *e = &s_cfg.sensors[s_cfg.nsensors++];
    e->dev_type = dev_type;
    e->dev_num = dev_num;
    e->trans_type = trans_type;
    config_save();
    return true;
}

void config_sensor_remove(int idx)
{
    if (idx < 0 || idx >= s_cfg.nsensors) return;
    memmove(&s_cfg.sensors[idx], &s_cfg.sensors[idx + 1], (s_cfg.nsensors - idx - 1) * sizeof s_cfg.sensors[0]);
    s_cfg.nsensors--;
    config_save();
}
