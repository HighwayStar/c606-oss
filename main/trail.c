/* Ridden path for the map page, see trail.h. */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "trail.h"
#include "mapfile.h"

static const char *TAG = "trail";

static SemaphoreHandle_t s_mtx;
static int32_t *s_x, *s_y;     /* zoom-20 pixels, PSRAM */
static size_t s_n;
static unsigned s_stride, s_skip;   /* decimation: keep one fix in `stride` */
static uint32_t s_gen;

static void lock(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_mtx); }

void trail_lock(void) { lock(); }
void trail_unlock(void) { unlock(); }

void trail_reset(void)
{
    lock();
    if (!s_x) {
        s_x = heap_caps_malloc(TRAIL_MAX_POINTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        s_y = heap_caps_malloc(TRAIL_MAX_POINTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        if (!s_x || !s_y) {
            ESP_LOGW(TAG, "no PSRAM for the trail");
            free(s_x); free(s_y);
            s_x = s_y = NULL;
        }
    }
    s_n = 0;
    s_stride = 1;
    s_skip = 0;
    s_gen++;
    unlock();
}

void trail_add(double lat, double lon)
{
    int32_t x = (int32_t)mapfile_lon_to_px(lon, 20), y = (int32_t)mapfile_lat_to_py(lat, 20);
    lock();
    if (!s_x) { unlock(); return; }
    if (s_n) {
        /* too close to the last kept point: GPS jitter while standing still */
        int32_t dx = x - s_x[s_n - 1], dy = y - s_y[s_n - 1];
        if (abs(dx) < TRAIL_MIN_PX && abs(dy) < TRAIL_MIN_PX) { unlock(); return; }
    }
    if (s_skip++ % s_stride) { unlock(); return; }
    if (s_n == TRAIL_MAX_POINTS) {
        /* full: keep every other point and halve the input rate from now on */
        for (size_t i = 0; i < s_n / 2; i++) {
            s_x[i] = s_x[2 * i];
            s_y[i] = s_y[2 * i];
        }
        s_n /= 2;
        s_stride *= 2;
        ESP_LOGI(TAG, "thinned to 1/%u", s_stride);
    }
    s_x[s_n] = x;
    s_y[s_n] = y;
    s_n++;
    s_gen++;
    unlock();
}

uint32_t trail_generation(void) { return s_gen; }

size_t trail_points(const int32_t **x20, const int32_t **y20)
{
    *x20 = s_x;
    *y20 = s_y;
    return s_n;
}
