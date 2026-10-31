#include "agnss.h"
#include "epo.h"
#include "gps.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "agnss";
/* Host and path recovered from the C606 vendor app; no account token needed. */
#define AGNSS_URL "https://rfs-fitness.rfsvr.net/indoor/v1/app/config/loda_agnss/download"
#define CACHE "/sdcard/c606oss/agnss.bin"
#define CACHE_TMP "/sdcard/c606oss/agnss.download"
#define CACHE_OLD "/sdcard/c606oss/agnss.previous"
#define MAX_ARCHIVE (256 * 1024)

static esp_err_t send_set(const uint8_t *data, unsigned *accepted)
{
    for (unsigned sv = 0; sv < EPO_GPS_RECORDS; sv++) {
        esp_err_t err = gps_assist_epo(data + sv * EPO_RECORD_SIZE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Receiver stopped at GPS EPO %u/32: %s", *accepted, esp_err_to_name(err));
            return err;
        }
        (*accepted)++;
    }
    return ESP_OK;
}

static esp_err_t apply(const uint8_t *archive, size_t size, uint32_t utc,
                       unsigned *accepted, uint32_t *expires)
{
    *accepted = 0;
    *expires = 0;
    if (utc < 1704067200) return ESP_ERR_INVALID_ARG;
    uint8_t *set = malloc(EPO_GPS_SET_SIZE);
    if (!set) return ESP_ERR_NO_MEM;
    int valid = epo_select_gps(archive, size, utc, set, expires);
    if (valid != 1) {
        ESP_LOGW(TAG, "EPO archive %s", valid == 0 ? "outside validity interval" : "malformed");
        free(set);
        return valid == 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "GPS Host EPO valid until Unix UTC %lu", (unsigned long)*expires);
    esp_err_t err = gps_assist_utc(utc);
    if (err == ESP_OK) err = send_set(set, accepted);
    free(set);
    return err;
}

static size_t read_cache(uint8_t *data)
{
    FILE *f = fopen(CACHE, "rb");
    if (!f) f = fopen(CACHE_OLD, "rb"); /* interrupted FAT rename */
    if (!f) return 0;
    size_t n = fread(data, 1, MAX_ARCHIVE, f);
    bool ok = !ferror(f) && feof(f);
    fclose(f);
    return ok ? n : 0;
}

static void save_cache(const uint8_t *data, size_t n)
{
    FILE *f = fopen(CACHE_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "Could not open EPO cache"); return; }
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        /* FatFS does not replace an existing destination in rename(). */
        remove(CACHE_OLD);
        if (rename(CACHE, CACHE_OLD) != 0 && errno != ENOENT) ok = false;
        if (ok && rename(CACHE_TMP, CACHE) != 0) {
            rename(CACHE_OLD, CACHE);
            ok = false;
        }
    }
    if (ok) { remove(CACHE_OLD); ESP_LOGI(TAG, "Cached %u bytes", (unsigned)n); }
    else { remove(CACHE_TMP); ESP_LOGW(TAG, "Could not save EPO cache"); }
}

static esp_err_t download(uint8_t *data, size_t *size)
{
    ESP_LOGI(TAG, "TLS memory: internal free=%u largest=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    esp_http_client_config_t cfg = {
        .url = AGNSS_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_open(client, 0);
    *size = 0;
    if (err == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200 || length > MAX_ARCHIVE) err = ESP_FAIL;
    }
    int64_t deadline = esp_timer_get_time() + 60000000;
    while (err == ESP_OK) {
        if (*size == MAX_ARCHIVE || esp_timer_get_time() > deadline) { err = ESP_ERR_INVALID_SIZE; break; }
        int n = esp_http_client_read(client, (char *)data + *size, MAX_ARCHIVE - *size);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_FAIL;
            break;
        }
        *size += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t agnss_update(unsigned *accepted, uint32_t *expires)
{
    *accepted = 0; *expires = 0;
    uint8_t *archive = heap_caps_malloc(MAX_ARCHIVE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!archive) return ESP_ERR_NO_MEM;
    size_t size = 0;
    esp_err_t err = download(archive, &size);
    bool fresh = err == ESP_OK;
    if (!fresh) {
        ESP_LOGW(TAG, "Download failed (%s); checking cache", esp_err_to_name(err));
        size = read_cache(archive);
    }
    time_t utc = time(NULL);
    if (size && utc >= 1704067200 && utc <= UINT32_MAX) {
        err = apply(archive, size, (uint32_t)utc, accepted, expires);
        if (err == ESP_OK && fresh) save_cache(archive, size);
    } else if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    free(archive);
    ESP_LOGI(TAG, "EPO result: %s, accepted %u/32", esp_err_to_name(err), *accepted);
    return err;
}

esp_err_t agnss_apply_cached(uint32_t utc, unsigned *accepted, uint32_t *expires)
{
    *accepted = 0; *expires = 0;
    uint8_t *archive = heap_caps_malloc(MAX_ARCHIVE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!archive) return ESP_ERR_NO_MEM;
    size_t size = read_cache(archive);
    esp_err_t err = size ? apply(archive, size, utc, accepted, expires) : ESP_ERR_NOT_FOUND;
    free(archive);
    return err;
}
