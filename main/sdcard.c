/* SD card on the SDMMC host, 4-bit, mounted at /sdcard.
 * Mirrors vendor MidVFSMount(): SDMMC_HOST_DEFAULT (slot 1, 20 MHz), custom
 * pins, internal pull-ups, 16 KB allocation unit. The vendor formats the card
 * on a failed mount; we never do. */
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_check.h"

#include "board.h"
#include "sdcard.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static sdcard_info_t s_info;

esp_err_t sdcard_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.d1 = SD_PIN_D1;
    slot.d2 = SD_PIN_D2;
    slot.d3 = SD_PIN_D3;
    slot.width = 4;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_info.mounted = true;
    strncpy(s_info.name, s_card->cid.name, sizeof s_info.name - 1);
    s_info.size_mb = ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20;
    s_info.high_speed = s_card->max_freq_khz > 26000;
    ESP_LOGI(TAG, "mounted %s: %s, %lu MB, %s, %d kHz", SD_MOUNT_POINT, s_info.name,
             (unsigned long)s_info.size_mb, s_card->is_sdio ? "SDIO" : s_card->is_mmc ? "MMC" : "SD",
             (int)s_card->max_freq_khz);

    /* peek at the root directory */
    DIR *d = opendir(SD_MOUNT_POINT);
    if (d) {
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) != NULL) {
            if (n++ < 12) {
                ESP_LOGI(TAG, "  %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
            }
        }
        closedir(d);
        s_info.root_entries = n;
    }
    return ESP_OK;
}

const sdcard_info_t *sdcard_info(void)
{
    return &s_info;
}
