/* Touch controller, as probed by the vendor firmware (C606 FUN_42031b14,
 * C706 FUN_420343d4: AXS15231 at 0x3B first, then FT6336, then CST328). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"

#include "board.h"
#include "touch.h"
#include "nrf_link.h"

static const char *TAG = "touch";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static touch_chip_t s_chip;

static esp_err_t add_dev(uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_dev);
}

static esp_err_t wr_rd(const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    return i2c_master_transmit_receive(s_dev, w, wn, r, rn, 100);
}

#if TOUCH_HAS_AXS15231
/* ---- AXS15231 touch (C706), vendor axs1523_read() @ 0x420358c0 ---------
 * The controller speaks 11-byte command frames. The vendor does not use the
 * public "B5 AB A5 5A 00 00 00 08" read alone; every poll is a handshake:
 *   1. write AB B5 5A A5 00 00 00 01 00 80 1F, read 1 byte: 0x05 = data ready
 *      (0x00 / 0x0A = nothing, counted as "read point error")
 *   2. write B5 AB A5 5A 00 00 00 0F 00 00 00, read 15 bytes
 *   3. write AB B5 5A A5 00 01 00 00 00 80 1F 0A (12 bytes, "consumed")
 * 15-byte record: [1] = points, [2] bit 6 (nibble == 4) = lift-off,
 * x = ([2]&0x0F)<<8 | [3], y = ([4]&0x0F)<<8 | [5], valid when [14] == [0];
 * 00 FF*13 F3 means "no touch". Init: read fw version (5A A5 AB B5 00 00 00
 * 01 00 80 89 -> 1 byte, expected > 0x12) and enable the "ESD firmware"
 * (B5 AB 5A A5 00 02 00 00 00 00 00 21 00). After 56 polls without a good
 * record the vendor power-cycles the module through the nRF (E2 02 09: 0, 1)
 * and re-enables. That blanks the panel too, so here it only logs (the
 * counter never got near the limit on the test unit). */
static const uint8_t axs_status_cmd[11] = {0xAB, 0xB5, 0x5A, 0xA5, 0x00, 0x00, 0x00, 0x01, 0x00, 0x80, 0x1F};
static const uint8_t axs_read_cmd[11]   = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00};
static const uint8_t axs_ack_cmd[12]    = {0xAB, 0xB5, 0x5A, 0xA5, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x1F, 0x0A};
static const uint8_t axs_fwver_cmd[11]  = {0x5A, 0xA5, 0xAB, 0xB5, 0x00, 0x00, 0x00, 0x01, 0x00, 0x80, 0x89};
static const uint8_t axs_esd_cmd[13]    = {0xB5, 0xAB, 0x5A, 0xA5, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00};
static const uint8_t axs_idle[15]       = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF3};
#define AXS_ESD_LIMIT 56
static int s_axs_bad;
static uint8_t s_axs_fw;

static esp_err_t axs_wr(const uint8_t *w, size_t n)
{
    return i2c_master_transmit(s_dev, w, n, 100);
}

static esp_err_t axs_rd(uint8_t *r, size_t n)
{
    return i2c_master_receive(s_dev, r, n, 100);
}

static void axs_esd_enable(void)
{
    if (axs_wr(axs_esd_cmd, sizeof axs_esd_cmd) != ESP_OK) {
        ESP_LOGW(TAG, "axs: esd firmware enable failed");
    }
}

static bool axs_probe(void)
{
    uint8_t buf[4];
    if (axs_rd(buf, 4) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        s_axs_fw = 0;
        if (axs_wr(axs_fwver_cmd, sizeof axs_fwver_cmd) == ESP_OK && axs_rd(&s_axs_fw, 1) == ESP_OK && s_axs_fw > 0x12) {
            break;
        }
        ESP_LOGW(TAG, "axs: fw version %u, retry %d", s_axs_fw, i);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    axs_esd_enable();
    s_chip = TOUCH_AXS15231;
    ESP_LOGI(TAG, "AXS15231 touch at 0x3B: fw %u", s_axs_fw);
    return true;
}

/* vendor "axs_tp_esd_num reset": module power off/on through the nRF
 * (nrf_link_send_lcd_power(0), 2 ms, (1), 150 ms, axs_esd_enable(), full
 * LVGL redraw). Off by default: it blanks the panel, and the counter only
 * grows when the controller stops answering. */
#ifndef AXS_ESD_RECOVERY
#define AXS_ESD_RECOVERY 0
#endif
static void axs_recover(void)
{
    ESP_LOGW(TAG, "axs: %d bad polls%s", s_axs_bad, AXS_ESD_RECOVERY ? ", power-cycling the module" : "");
    s_axs_bad = 0;
#if AXS_ESD_RECOVERY
    nrf_link_send_lcd_power(0);
    vTaskDelay(pdMS_TO_TICKS(2));
    nrf_link_send_lcd_power(1);
    vTaskDelay(pdMS_TO_TICKS(150));
    axs_esd_enable();
    lv_obj_invalidate(lv_screen_active());   /* the panel came up blank */
#endif
}

static bool axs_read(uint16_t *x, uint16_t *y)
{
    uint8_t st = 0, buf[15];
    bool ok = axs_wr(axs_status_cmd, sizeof axs_status_cmd) == ESP_OK && axs_rd(&st, 1) == ESP_OK;
    if (!ok || st != 0x05) {
        if (++s_axs_bad > AXS_ESD_LIMIT) axs_recover();
        return false;
    }
    ok = axs_wr(axs_read_cmd, sizeof axs_read_cmd) == ESP_OK && axs_rd(buf, sizeof buf) == ESP_OK;
    axs_wr(axs_ack_cmd, sizeof axs_ack_cmd);
    if (!ok) {
        if (++s_axs_bad > AXS_ESD_LIMIT) axs_recover();
        return false;
    }
    if (!memcmp(buf, axs_idle, sizeof buf)) {
        s_axs_bad = 0;
        return false;
    }
    uint8_t sum = 0;
    for (int i = 0; i < 14; i++) sum += buf[i];
    if (sum != buf[14]) {
        s_axs_bad++;
        ESP_LOGD(TAG, "axs: bad checksum %02x != %02x", sum, buf[14]);
        return false;
    }
    uint8_t n = buf[1];
    if (n < 1 || n > 2 || (buf[2] >> 4) == 4) {
        s_axs_bad = 0;
        return false;   /* no point or lift-off */
    }
    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    if (*x > LCD_H_RES || *y > LCD_V_RES) {
        return false;
    }
    s_axs_bad = 0;
    return true;
}
#endif

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "bus");

    uint8_t reg, buf[8];
    (void)reg; (void)buf;
#if TOUCH_HAS_AXS15231
    if (i2c_master_probe(s_bus, TOUCH_ADDR_AXS15231, 50) == ESP_OK && add_dev(TOUCH_ADDR_AXS15231) == ESP_OK) {
        if (axs_probe()) {
            return ESP_OK;
        }
        i2c_master_bus_rm_device(s_dev);
    }
#endif
#if TOUCH_HAS_FT6X36
    /* FocalTech FT6x36 */
    if (i2c_master_probe(s_bus, TOUCH_ADDR_FT6X36, 50) == ESP_OK && add_dev(TOUCH_ADDR_FT6X36) == ESP_OK) {
        reg = 0xA8;
        if (wr_rd(&reg, 1, buf, 1) == ESP_OK) {
            uint8_t vendor = buf[0];
            uint8_t mode[2] = {0x00, 0x00};        /* device mode = normal */
            i2c_master_transmit(s_dev, mode, 2, 100);
            reg = 0xA6; wr_rd(&reg, 1, &buf[1], 1);   /* firmware version */
            reg = 0xA3; wr_rd(&reg, 1, &buf[2], 1);   /* chip id */
            s_chip = TOUCH_FT6X36;
            ESP_LOGI(TAG, "FT6x36 at 0x38: vendor 0x%02x fw 0x%02x chip 0x%02x", vendor, buf[1], buf[2]);
            return ESP_OK;
        }
        i2c_master_bus_rm_device(s_dev);
    }
#endif
#if TOUCH_HAS_CST328
    /* Hynitron CST328 */
    if (i2c_master_probe(s_bus, TOUCH_ADDR_CST328, 50) == ESP_OK && add_dev(TOUCH_ADDR_CST328) == ESP_OK) {
        const uint8_t r45[] = {0xD0, 0x45};
        if (wr_rd(r45, 2, buf, 4) == ESP_OK) {
            s_chip = TOUCH_CST328;
            ESP_LOGI(TAG, "CST328 at 0x5A: %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);
            return ESP_OK;
        }
        i2c_master_bus_rm_device(s_dev);
    }
#endif
    ESP_LOGW(TAG, "no touch controller found on I2C%d (sda %d scl %d)", TOUCH_I2C_PORT, TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    return ESP_ERR_NOT_FOUND;
}

touch_chip_t touch_chip(void) { return s_chip; }

const char *touch_chip_name(void)
{
    return s_chip == TOUCH_FT6X36 ? "FT6x36" : s_chip == TOUCH_CST328 ? "CST328"
         : s_chip == TOUCH_AXS15231 ? "AXS15231" : "none";
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t buf[8];
#if TOUCH_HAS_AXS15231
    if (s_chip == TOUCH_AXS15231) {
        return axs_read(x, y);
    }
#endif
    if (s_chip == TOUCH_FT6X36) {
        /* vendor: read reg 0x02 -> [count, XH, XL, YH, YL]; pressed if count == 1 */
        uint8_t reg = 0x02;
        if (wr_rd(&reg, 1, buf, 5) != ESP_OK || (buf[0] & 0x0F) != 1) {
            return false;
        }
        *x = ((buf[1] & 0x0F) << 8) | buf[2];
        *y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }
    if (s_chip == TOUCH_CST328) {
        /* vendor: read 0xD000 (7 bytes), pressed if (b0 & 0x0F) == 6, then write 0xD000AB */
        const uint8_t r00[] = {0xD0, 0x00};
        const uint8_t ack[] = {0xD0, 0x00, 0xAB};
        bool ok = wr_rd(r00, 2, buf, 7) == ESP_OK;
        i2c_master_transmit(s_dev, ack, 3, 100);
        if (!ok || (buf[0] & 0x0F) != 6) {
            return false;
        }
        *x = ((uint16_t)buf[1] << 4) | (buf[3] >> 4);
        *y = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);
        return true;
    }
    return false;
}
