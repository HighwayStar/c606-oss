#include "esp_log.h"
#include "esp_check.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

#include "sdcard.h"
#include "tracklog.h"
#include "usb_msc.h"

static const char *TAG = "usb";
static bool s_active;
static sdmmc_card_t s_card;

static const char *k_strings[] = {
    (const char[]){0x09, 0x04},   /* 0: language, English */
    "c606-oss",                   /* 1: manufacturer */
    "C606 eMMC",                  /* 2: product */
    "0001",                       /* 3: serial */
};

esp_err_t usb_msc_enter(void)
{
    if (s_active) return ESP_OK;

    tracklog_stop();
    ESP_RETURN_ON_ERROR(sdcard_unmount(), TAG, "unmount");

    /* re-open the card for TinyUSB's block-level access */
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sdcard_host_config(&host, &slot);
    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "slot init");
    ESP_RETURN_ON_ERROR(sdmmc_card_init(&host, &s_card), TAG, "card init");

    tinyusb_msc_sdmmc_config_t msc = { .card = &s_card };
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&msc), TAG, "msc storage");
    /* not mounted for the app => exposed to the host */

    tinyusb_config_t tusb = {
        .device_descriptor = NULL,
        .string_descriptor = k_strings,
        .string_descriptor_count = sizeof k_strings / sizeof k_strings[0],
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb), TAG, "tinyusb");
    s_active = true;
    ESP_LOGI(TAG, "USB mass storage active (console will now go away)");
    return ESP_OK;
}

bool usb_msc_active(void)
{
    return s_active;
}
