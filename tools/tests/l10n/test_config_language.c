#include "config.h"
#include "l10n.h"
#include "nvs.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static app_cfg_t stored;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    assert(!strcmp(ns, "c606oss")); (void)mode; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h; assert(!strcmp(key, "cfg") && *size >= sizeof stored);
    memcpy(out, &stored, sizeof stored); *size = sizeof stored; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size)
{
    (void)h; assert(!strcmp(key, "cfg") && size == sizeof stored);
    memcpy(&stored, in, size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

int main(void)
{
    _Static_assert(sizeof(app_cfg_t) == 192, "binary config layout");
    _Static_assert(offsetof(app_cfg_t, language) == 191, "language must only use old tail padding");
    config_defaults(&stored);
    stored.version = 14;
    stored.language = 0xff; /* previous tail padding must never become a language */
    stored.tz_min = 180;
    stored.weight_kg = 91;
    stored.wheel_mm = 2150;
    stored.debug_log = 1;
    strcpy(stored.route, "saved.gpx");
    app_cfg_t old = stored;
    config_load();
    app_cfg_t expected = old;
    expected.version = 15;
    expected.language = LANG_EN;
    assert(!memcmp(config_get(), &expected, sizeof expected));
    config_get()->language = LANG_RU;
    assert(config_save() == ESP_OK);
    config_load();
    assert(config_get()->language == LANG_RU && config_get()->weight_kg == 91);
    assert(!strcmp(config_get()->route, "saved.gpx"));
    stored.language = LANG_COUNT;
    config_load();
    assert(config_get()->language == LANG_EN);
    puts("PASS: v14 migration preserves all settings; language persists; invalid values rejected");
}
