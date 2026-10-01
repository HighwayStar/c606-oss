/* Debug log on the eMMC, see dbglog.h. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"

#include "board.h"
#include "sdcard.h"
#include "utc.h"
#include "dbglog.h"

static const char *TAG = "dbglog";

#define RING_SIZE  (64 * 1024)   /* PSRAM; ~1 min of a busy log */
#define LINE_MAX   256           /* longer lines are truncated */
#define CHUNK      4096          /* ring -> file in pieces of this size */
#define FLUSH_MS   1000

static vprintf_like_t s_prev;
static SemaphoreHandle_t s_ring_mtx;   /* ring + line buffer, held briefly by any logging task */
static SemaphoreHandle_t s_file_mtx;   /* the file, held by the writer while it writes */
static char *s_ring, *s_chunk;
static uint32_t s_head, s_tail;        /* free-running byte counters */
static volatile uint32_t s_dropped;    /* lines lost: ring full or ring busy */
static char s_line[LINE_MAX];
static volatile bool s_on, s_shutdown;
static TaskHandle_t s_task;
static char s_path[sizeof DBGLOG_DIR + 24];   /* current file, "" = none yet */
static uint32_t s_file_bytes;
static bool s_boot_header_done;
static uint32_t s_last_utc_min;

/* Appends one line without the ANSI colour sequences (ESC [ ... m); no
 * line buffer on the stack, this runs in whatever task logs. */
static void ring_put(const char *p, int n)
{
    if (RING_SIZE - (s_head - s_tail) < (uint32_t)n) {
        s_dropped++;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (p[i] == 0x1B) {
            while (i < n && p[i] != 'm') i++;
            continue;
        }
        s_ring[s_head++ % RING_SIZE] = p[i];
    }
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int ret = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    if (s_on && s_ring && !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        /* short timeout: a logging task must never stall on the file system */
        if (xSemaphoreTake(s_ring_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            int n = vsnprintf(s_line, sizeof s_line, fmt, ap2);
            if (n >= (int)sizeof s_line) {
                n = sizeof s_line - 1;
                s_line[n - 1] = '\n';
            }
            if (n > 0) ring_put(s_line, n);
            uint32_t fill = s_head - s_tail;
            xSemaphoreGive(s_ring_mtx);
            if (fill > RING_SIZE / 2 && s_task) xTaskNotifyGive(s_task);
        } else {
            s_dropped++;
        }
    }
    va_end(ap2);
    return ret;
}

/* Adds a line of our own (header, UTC marker) to the ring. */
static void ring_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    ring_put(buf, n);
    xSemaphoreGive(s_ring_mtx);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
    }
}

static bool utc_text(char *buf, size_t n)
{
    uint32_t now;
    if (!utc_now(&now)) return false;
    time_t t = now;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return true;
}

static int file_index(const char *name)
{
    int idx;
    char tail[8];
    if (sscanf(name, "log%4d.%3s", &idx, tail) == 2 && !strcmp(tail, "txt")) return idx;
    return -1;
}

/* Creates the next file (number after the newest one) with its header; the
 * oldest beyond DBGLOG_KEEP go. The file is opened only while appending:
 * with CONFIG_FATFS_FS_LOCK a file open for writing cannot be opened by
 * anyone else (devcon get, USB), and close() commits it to the card. */
static bool new_file(void)
{
    mkdir("/sdcard/c606oss", 0777);
    mkdir(DBGLOG_DIR, 0777);
    int max = 0;
    DIR *d = opendir(DBGLOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int i = file_index(e->d_name);
            if (i > max) max = i;
        }
        rewinddir(d);
        while ((e = readdir(d)) != NULL) {
            int i = file_index(e->d_name);
            if (i > 0 && i <= max + 1 - DBGLOG_KEEP) {
                char path[sizeof DBGLOG_DIR + 24];
                snprintf(path, sizeof path, DBGLOG_DIR "/log%04d.txt", i);
                unlink(path);
            }
        }
        closedir(d);
    }
    snprintf(s_path, sizeof s_path, DBGLOG_DIR "/log%04d.txt", max >= 9999 ? 1 : max + 1);
    int fd = open(s_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        s_path[0] = 0;
        return false;
    }
    s_file_bytes = 0;

    char utc[32] = "time unknown";
    utc_text(utc, sizeof utc);
    const esp_app_desc_t *app = esp_app_get_description();
    char hdr[200];
    int n;
    if (!s_boot_header_done) {
        n = snprintf(hdr, sizeof hdr, "=== %s open firmware %s (%s %s), reset: %s, up %lu s, %s\n", BOARD_NAME,
                     app->version, app->date, app->time, reset_reason(),
                     (unsigned long)(esp_timer_get_time() / 1000000), utc);
        s_boot_header_done = true;
    } else {
        n = snprintf(hdr, sizeof hdr, "=== continued, up %lu s, %s\n",
                     (unsigned long)(esp_timer_get_time() / 1000000), utc);
    }
    bool ok = write(fd, hdr, n) == n;
    if (ok) s_file_bytes += n;
    close(fd);
    return ok;
}

/* Ring -> end of the current file. Called with s_file_mtx held. */
static bool drain(void)
{
    if (!s_path[0]) return false;
    if (s_head == s_tail && !s_dropped) return true;
    int fd = open(s_path, O_WRONLY | O_APPEND);
    if (fd < 0) return false;
    bool ok = true;
    if (s_dropped) {
        char buf[48];
        int n = snprintf(buf, sizeof buf, "[dbglog: %lu lines dropped]\n", (unsigned long)s_dropped);
        s_dropped = 0;
        if (write(fd, buf, n) == n) s_file_bytes += n;
        else ok = false;
    }
    while (ok) {
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        uint32_t n = s_head - s_tail;
        if (n > CHUNK) n = CHUNK;
        for (uint32_t i = 0; i < n; i++) s_chunk[i] = s_ring[(s_tail + i) % RING_SIZE];
        s_tail += n;
        xSemaphoreGive(s_ring_mtx);
        if (!n) break;
        if (write(fd, s_chunk, n) != (ssize_t)n) ok = false;
        else s_file_bytes += n;
    }
    if (close(fd) != 0) ok = false;
    return ok;
}

static void writer_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_MS));
        if (s_on) {
            uint32_t now;
            if (utc_now(&now) && now / 60 != s_last_utc_min) {
                s_last_utc_min = now / 60;
                char utc[32];
                utc_text(utc, sizeof utc);
                ring_printf("[dbglog] %s = up %lu ms\n", utc, (unsigned long)(esp_timer_get_time() / 1000));
            }
        }
        xSemaphoreTake(s_file_mtx, portMAX_DELAY);
        if (!s_shutdown && sdcard_info()->mounted) {
            if (s_on && !s_path[0] && !new_file()) ESP_LOGW(TAG, "cannot create a log file in " DBGLOG_DIR);
            if (s_path[0]) {
                if (!drain()) {
                    s_path[0] = 0;   /* card trouble: try a new file next time */
                } else if (!s_on) {
                    s_path[0] = 0;   /* switched on again later: a new file */
                } else if (s_file_bytes >= DBGLOG_FILE_MAX) {
                    new_file();
                }
            }
        }
        xSemaphoreGive(s_file_mtx);
    }
}

void dbglog_set_enabled(bool on)
{
    if (on && !s_ring) {
        s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
        s_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
        if (!s_ring || !s_chunk) {
            free(s_ring);
            free(s_chunk);
            s_ring = s_chunk = NULL;
            ESP_LOGE(TAG, "no PSRAM for the log buffer");
            return;
        }
    }
    if (on && !s_task) {
        /* below the UI and the sensor tasks; FatFS needs an internal stack */
        xTaskCreate(writer_task, "dbglog", 4096, NULL, 1, &s_task);
    }
    if (on == s_on) return;
    if (on) {
        s_on = true;
        ESP_LOGI(TAG, "debug log on, " DBGLOG_DIR);
    } else {
        ESP_LOGI(TAG, "debug log off");
        s_on = false;   /* the writer flushes what is left and closes the file */
    }
    if (s_task) xTaskNotifyGive(s_task);
}

bool dbglog_enabled(void) { return s_on; }

void dbglog_init(bool enabled)
{
    s_ring_mtx = xSemaphoreCreateMutex();
    s_file_mtx = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(log_hook);
    if (enabled) dbglog_set_enabled(true);
}

void dbglog_shutdown(void)
{
    if (!s_file_mtx) return;
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    if (s_path[0] && sdcard_info()->mounted) drain();
    s_shutdown = true;
    xSemaphoreGive(s_file_mtx);
}
