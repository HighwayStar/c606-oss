#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "fit.h"

static const char *TAG = "fit";

#define FIT_HEADER_SIZE   14
#define FIT_PROTOCOL_VER  0x20      /* 2.0 */
#define FIT_PROFILE_VER   2132      /* 21.32 */

uint16_t fit_crc(uint16_t crc, const void *data, size_t len)
{
    static const uint16_t tab[16] = {
        0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
        0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
    };
    const uint8_t *p = data;
    while (len--) {
        uint8_t b = *p++;
        uint16_t t = tab[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ t ^ tab[b & 0xF];
        t = tab[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ t ^ tab[(b >> 4) & 0xF];
    }
    return crc;
}

static void put_header(uint8_t *h, uint32_t data_size)
{
    h[0] = FIT_HEADER_SIZE;
    h[1] = FIT_PROTOCOL_VER;
    h[2] = FIT_PROFILE_VER & 0xFF;
    h[3] = FIT_PROFILE_VER >> 8;
    h[4] = data_size & 0xFF;
    h[5] = (data_size >> 8) & 0xFF;
    h[6] = (data_size >> 16) & 0xFF;
    h[7] = data_size >> 24;
    memcpy(h + 8, ".FIT", 4);
    uint16_t crc = fit_crc(0, h, 12);
    h[12] = crc & 0xFF;
    h[13] = crc >> 8;
}

static esp_err_t emit(fit_writer_t *w, const void *data, size_t len)
{
    if (fwrite(data, 1, len, w->f) != len) {
        ESP_LOGE(TAG, "write failed");
        return ESP_FAIL;
    }
    w->crc = fit_crc(w->crc, data, len);
    w->data_size += len;
    return ESP_OK;
}

esp_err_t fit_open(fit_writer_t *w, const char *path)
{
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f) {
        return ESP_FAIL;
    }
    uint8_t h[FIT_HEADER_SIZE];
    put_header(h, 0);
    if (fwrite(h, 1, sizeof h, w->f) != sizeof h) {
        fclose(w->f);
        w->f = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t fit_mesg_size(const fit_mesg_t *m)
{
    size_t n = 0;
    for (int i = 0; i < m->nfields; i++) n += m->fields[i].size;
    return n;
}

static esp_err_t write_definition(fit_writer_t *w, uint8_t local, const fit_mesg_t *m)
{
    uint8_t hdr[6] = {
        0x40 | local,          /* definition message, normal header */
        0,                     /* reserved */
        0,                     /* architecture: little-endian */
        m->global & 0xFF, m->global >> 8,
        m->nfields,
    };
    esp_err_t err = emit(w, hdr, sizeof hdr);
    for (int i = 0; i < m->nfields && err == ESP_OK; i++) {
        uint8_t fd[3] = { m->fields[i].num, m->fields[i].size, m->fields[i].type };
        err = emit(w, fd, sizeof fd);
    }
    if (err == ESP_OK) w->local[local] = m;
    return err;
}

esp_err_t fit_write(fit_writer_t *w, uint8_t local, const fit_mesg_t *m, const void *payload, size_t len)
{
    if (!w->f || local >= FIT_LOCAL_MESGS) return ESP_ERR_INVALID_STATE;
    if (len != fit_mesg_size(m)) {
        ESP_LOGE(TAG, "mesg %u: payload %u bytes, definition %u", m->global, (unsigned)len, (unsigned)fit_mesg_size(m));
        return ESP_ERR_INVALID_SIZE;
    }
    if (w->local[local] != m) {
        esp_err_t err = write_definition(w, local, m);
        if (err != ESP_OK) return err;
    }
    uint8_t hdr = local;       /* data message, normal header */
    esp_err_t err = emit(w, &hdr, 1);
    if (err == ESP_OK) err = emit(w, payload, len);
    return err;
}

void fit_sync(fit_writer_t *w)
{
    if (!w->f) return;
    fflush(w->f);
    fsync(fileno(w->f));
}

esp_err_t fit_close(fit_writer_t *w)
{
    if (!w->f) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    uint8_t crc[2] = { w->crc & 0xFF, w->crc >> 8 };
    if (fwrite(crc, 1, 2, w->f) != 2) err = ESP_FAIL;
    uint8_t h[FIT_HEADER_SIZE];
    put_header(h, w->data_size);
    if (fseek(w->f, 0, SEEK_SET) != 0 || fwrite(h, 1, sizeof h, w->f) != sizeof h) err = ESP_FAIL;
    fflush(w->f);
    fsync(fileno(w->f));
    if (fclose(w->f) != 0) err = ESP_FAIL;
    w->f = NULL;
    return err;
}
