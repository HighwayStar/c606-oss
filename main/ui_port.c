/*
 * LVGL 9 glue: display driver on top of lcd.c, tick source, render task and
 * a lock so other tasks can touch widgets.
 *
 * Buffers mirror the vendor's LVGL 8 setup: two 60-line RGB565 buffers in
 * internal DMA-capable RAM, partial render mode.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "lvgl.h"

#include "board.h"
#include "lcd.h"
#include "ui_port.h"

static const char *TAG = "ui_port";

static lv_display_t *s_disp;
static SemaphoreHandle_t s_lock;

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    lcd_draw_bitmap(a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    /* lv_display_flush_ready() is called from the DMA-done ISR */
}

static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        ui_lock();
        uint32_t wait_ms = lv_timer_handler();
        ui_unlock();
        if (wait_ms > 50) wait_ms = 50;
        if (wait_ms < 1) wait_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t ui_port_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    void *buf1 = heap_caps_malloc(LCD_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LCD_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf1, buf2, LCD_MAX_TRANSFER, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lcd_set_done_cb(on_flush_done, NULL);

    /* LVGL needs a fat stack; keep it in internal RAM (default). */
    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16 * 1024, NULL, 5, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");
    return ESP_OK;
}

void ui_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}
