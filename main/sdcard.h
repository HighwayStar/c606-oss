#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/sdmmc_host.h"

#define SD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    char name[8];
    uint32_t size_mb;
    bool high_speed;
    int root_entries;
} sdcard_info_t;

/* Host/slot settings shared with the USB MSC path. */
void sdcard_host_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot);

esp_err_t sdcard_mount(void);
/* Unmount the FAT and release the SDMMC host. */
esp_err_t sdcard_unmount(void);
const sdcard_info_t *sdcard_info(void);
