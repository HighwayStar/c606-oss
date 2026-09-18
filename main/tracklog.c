#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "sdcard.h"
#include "tracklog.h"

static const char *TAG = "track";
#define TRACK_DIR SD_MOUNT_POINT "/c606oss"

static FILE *s_f;
static char s_name[64];
static uint32_t s_points;
static uint32_t s_unsynced;
static SemaphoreHandle_t s_lock;

esp_err_t tracklog_start(const gps_fix_t *fix)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!sdcard_info()->mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_f) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    mkdir(TRACK_DIR, 0775);
    if (fix->year) {
        snprintf(s_name, sizeof s_name, TRACK_DIR "/20%02u%02u%02u-%02u%02u%02u.csv",
                 fix->year, fix->mon, fix->day, fix->hh, fix->mm, fix->ss);
    } else {
        /* no GPS time yet: pick the next free numbered name */
        struct stat st;
        for (int i = 1; i < 1000; i++) {
            snprintf(s_name, sizeof s_name, TRACK_DIR "/track-%03d.csv", i);
            if (stat(s_name, &st) != 0) break;
        }
    }
    s_f = fopen(s_name, "w");
    if (!s_f) {
        ESP_LOGW(TAG, "cannot create %s", s_name);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    fputs("utc,lat,lon,alt_m,speed_kmh,course,sats,hdop,temp_c,pressure_hpa\n", s_f);
    s_points = 0;
    s_unsynced = 0;
    ESP_LOGI(TAG, "recording to %s", s_name);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void tracklog_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_f) {
        fclose(s_f);
        s_f = NULL;
        ESP_LOGI(TAG, "closed %s, %lu points", s_name, (unsigned long)s_points);
    }
    xSemaphoreGive(s_lock);
}

void tracklog_flush(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_f) {
        fflush(s_f);
        fsync(fileno(s_f));
        s_unsynced = 0;
    }
    xSemaphoreGive(s_lock);
}

bool tracklog_active(void) { return s_f != NULL; }
uint32_t tracklog_points(void) { return s_points; }
const char *tracklog_filename(void) { return s_name; }

void tracklog_point(const gps_fix_t *fix, int16_t temp_c100, uint32_t pressure_pa100)
{
    if (!s_f || !fix->valid) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_f) {
        fprintf(s_f, "20%02u-%02u-%02uT%02u:%02u:%02uZ,%.6f,%.6f,%.1f,%.1f,%.0f,%u,%.1f,%.2f,%.2f\n",
                fix->year, fix->mon, fix->day, fix->hh, fix->mm, fix->ss,
                fix->lat, fix->lon, fix->alt_m, fix->speed_kmh, fix->course_deg,
                fix->sats_used, fix->hdop, temp_c100 / 100.0, pressure_pa100 / 10000.0);
        s_points++;
        /* fatfs buffers in RAM; push to the card every 10 points so a
         * power loss costs at most ~10 s of track */
        if (++s_unsynced >= 10) {
            s_unsynced = 0;
            fflush(s_f);
            fsync(fileno(s_f));
        }
    }
    xSemaphoreGive(s_lock);
}
