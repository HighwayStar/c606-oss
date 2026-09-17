/* Touch controller, as probed by the vendor firmware (FUN_42031b14). */
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

#include "board.h"
#include "touch.h"

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
    ESP_LOGW(TAG, "no touch controller found on I2C%d (sda %d scl %d)", TOUCH_I2C_PORT, TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    return ESP_ERR_NOT_FOUND;
}

touch_chip_t touch_chip(void) { return s_chip; }

const char *touch_chip_name(void)
{
    return s_chip == TOUCH_FT6X36 ? "FT6x36" : s_chip == TOUCH_CST328 ? "CST328" : "none";
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t buf[8];
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
