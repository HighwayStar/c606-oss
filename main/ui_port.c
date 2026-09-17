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
#include "touch.h"
#include "ui_port.h"

static const char *TAG = "ui_port";

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lock;
static int16_t s_last_x, s_last_y;
static bool s_pressed;

/* Raw controller coordinates -> screen. Vendor uses them as-is for the
 * 240x320 panel; adjust here if the axes turn out mirrored/swapped. */
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 0
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 0
#endif

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        if (TOUCH_SWAP_XY) { uint16_t t = x; x = y; y = t; }
        if (TOUCH_MIRROR_X) x = LCD_H_RES - 1 - x;
        if (TOUCH_MIRROR_Y) y = LCD_V_RES - 1 - y;
        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;
        s_last_x = x;
        s_last_y = y;
        s_pressed = true;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        s_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
}

bool ui_port_touch_state(int16_t *x, int16_t *y)
{
    *x = s_last_x;
    *y = s_last_y;
    return s_pressed;
}

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

    if (touch_chip() != TOUCH_NONE) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
    }

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
