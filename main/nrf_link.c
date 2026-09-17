/*
 * UART link to the nRF co-processor.
 *
 * Reverse engineered from the vendor firmware:
 *   - MidCommInit(1, 115200, ...)     -> UART2, TX GPIO42, RX GPIO41
 *   - UartDataSaveToRingBuffer()      -> 10 ms timer copying UART into a ring
 *   - ComRecvTask (FUN_42051330)      -> frame sync / CRC / dispatch
 *   - FUN_42051260                    -> TX frame builder
 *   - FUN_4222c01c                    -> CRC-16/XMODEM
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"

#include "board.h"
#include "nrf_link.h"

static const char *TAG = "nrf";

static nrf_frame_cb_t s_cb;
static void *s_cb_ctx;
static SemaphoreHandle_t s_tx_lock;

/* Vendor CRC (FUN_4222c01c) is a bit-serial augmented CRC with 16 trailing
 * zero bits (two Xtensa `loop 8` blocks the decompiler hides), i.e. plain
 * CRC-16/XMODEM: poly 0x1021, init 0, no reflection, check("123456789") = 0x31C3. */
uint16_t nrf_link_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t nrf_link_send(uint8_t type, uint8_t cmd, const void *payload, uint8_t len)
{
    if (len + 8 > NRF_MAX_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t f[NRF_MAX_FRAME];
    size_t total = len + 8;
    f[0] = NRF_SYNC;
    f[1] = len + 4;
    f[2] = NRF_MARK;
    f[3] = NRF_DIR_ESP_TO_NRF;
    f[4] = type;
    f[5] = cmd;
    memcpy(&f[6], payload, len);
    uint16_t crc = nrf_link_crc16(f, total - 2);
    f[total - 2] = crc & 0xFF;
    f[total - 1] = crc >> 8;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = uart_write_bytes(NRF_UART_NUM, f, total);
    xSemaphoreGive(s_tx_lock);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, f, total, ESP_LOG_DEBUG);
    return n == (int)total ? ESP_OK : ESP_FAIL;
}

esp_err_t nrf_link_send_ctrl(uint8_t type, uint8_t grp, uint8_t id, uint8_t val)
{
    uint8_t p[8] = {NRF_SYS_CTRL, grp, id, 0, 0, val, 0, 0};
    return nrf_link_send(type, NRF_CMD_SYS, p, sizeof p);
}

esp_err_t nrf_link_send_power_on(void)
{
    return nrf_link_send_ctrl(NRF_TYPE_SET, 0x02, 0x00, 0x01);
}

bool nrf_link_decode_key(const uint8_t *f, size_t len, nrf_key_event_t *ev)
{
    /* payload = f[6..], needs at least 7 payload bytes */
    if (len < 6 + 7 + 2 || f[5] != NRF_CMD_SYS || f[6] != NRF_SYS_BUTTON) {
        return false;
    }
    ev->key = f[7];
    ev->aux = f[11];
    ev->event = f[12];
    return true;
}

/* Frame sync state machine, same acceptance rules as the vendor task:
 *   need >= 8 bytes; f[0]==0xA5, f[2]=='o', 1 <= f[4] <= 5, f[1] < 0x85,
 *   total = f[1]+4, CRC16 over total-2 bytes == LE u16 at the end.
 * Anything else: discard one byte and resync. */
static void link_task(void *arg)
{
    static uint8_t buf[512];
    size_t fill = 0;

    for (;;) {
        int n = uart_read_bytes(NRF_UART_NUM, buf + fill, sizeof buf - fill, pdMS_TO_TICKS(20));
        if (n > 0) {
            fill += n;
        }
        size_t pos = 0;
        while (fill - pos >= 8) {
            const uint8_t *f = buf + pos;
            if (f[0] != NRF_SYNC || f[2] != NRF_MARK || f[4] < 1 || f[4] > 5 || f[1] >= 0x85) {
                pos++;
                continue;
            }
            size_t total = (size_t)f[1] + 4;
            if (fill - pos < total) {
                break; /* wait for the rest */
            }
            uint16_t want = f[total - 2] | (f[total - 1] << 8);
            if (nrf_link_crc16(f, total - 2) != want) {
                ESP_LOGW(TAG, "bad crc, resync");
                pos++;
                continue;
            }
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, f, total, ESP_LOG_DEBUG);
            if (s_cb) {
                s_cb(f, total, s_cb_ctx);
            }
            pos += total;
        }
        if (pos) {
            memmove(buf, buf + pos, fill - pos);
            fill -= pos;
        }
        if (fill == sizeof buf) {
            fill = 0; /* garbage with no sync; drop it */
        }
    }
}

esp_err_t nrf_link_init(nrf_frame_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tx_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    uart_config_t cfg = {
        .baud_rate = NRF_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* vendor: rx buf 0x400, tx buf 0x108, no event queue */
    ESP_RETURN_ON_ERROR(uart_driver_install(NRF_UART_NUM, 1024, 1024, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(NRF_UART_NUM, &cfg), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(NRF_UART_NUM, NRF_UART_TX, NRF_UART_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");

    xTaskCreate(link_task, "nrf_link", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART%d tx=%d rx=%d @ %d", NRF_UART_NUM, NRF_UART_TX, NRF_UART_RX, NRF_UART_BAUD);
    return ESP_OK;
}
