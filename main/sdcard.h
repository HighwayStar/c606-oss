#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    char name[8];
    uint32_t size_mb;
    bool high_speed;
    int root_entries;
} sdcard_info_t;

esp_err_t sdcard_mount(void);
const sdcard_info_t *sdcard_info(void);
