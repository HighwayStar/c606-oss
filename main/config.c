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
#define CFG_VERSION 2
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

    static const field_id_t p1[] = {
        FIELD_TIME_OF_DAY, FIELD_STAT(STAT_SPEED, AGG_CUR), FIELD_STAT(STAT_SPEED, AGG_AVG),
        FIELD_STAT(STAT_HR, AGG_CUR), FIELD_STAT(STAT_CADENCE, AGG_CUR),
        FIELD_SESSION_TIME, FIELD_BATTERY_PCT,
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
        FIELD_TIME_OF_DAY, FIELD_STAT(STAT_TEMP, AGG_MIN), FIELD_STAT(STAT_TEMP, AGG_MAX),
        FIELD_STAT(STAT_ALTITUDE, AGG_MAX),
    };
    set_page(&c->page[0], true, "7A", p1, sizeof p1);
    set_page(&c->page[1], true, "6C", p2, sizeof p2);
    set_page(&c->page[2], true, "12", p3, sizeof p3);
    set_page(&c->page[3], false, "4A", p4, sizeof p4);
    set_page(&c->page[4], false, "1", p1, 1);
}

static bool valid(const app_cfg_t *c)
{
    if (c->magic != CFG_MAGIC || c->version != CFG_VERSION) return false;
    if (c->theme > 1) return false;
    for (int p = 0; p < CFG_PAGES; p++) {
        if (c->page[p].layout >= layout_count()) return false;
        for (int i = 0; i < LAYOUT_MAX_CELLS; i++) {
            if (c->page[p].field[i] >= FIELD_COUNT) return false;
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
    /* version 1 blobs end right before `theme`: upgrade in place */
    if (err == ESP_OK && tmp.magic == CFG_MAGIC && tmp.version == 1 && len == offsetof(app_cfg_t, theme)) {
        tmp.version = 2;
        tmp.theme = 0;
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
