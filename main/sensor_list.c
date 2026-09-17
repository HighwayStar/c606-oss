/* Reads the vendor's paired-sensor file so the open firmware can reuse the
 * pairing. Format (CONFIG/sensor_list.json):
 *   {"Ver":1,"Cnt":4,"DevInfo":[{"ConnType":0,"DevType":1,"DevID":"6898-1",...},...]}
 * ConnType 0 = ANT+, 1 = BLE. DevType is the vendor's internal index
 * (FUN_421b92b0): 0 HR, 1 cadence, 2 speed, 3 spd+cad, 4 power, 5 trainer,
 * 6 radar, 7 shifting, 8 light. DevID = "<device number>-<transmission type>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"

#include "sdcard.h"
#include "ant.h"
#include "sensor_list.h"

static const char *TAG = "sensors";

static const uint8_t k_vendor_type_to_ant[] = {
    ANT_DEV_HR, ANT_DEV_CADENCE, ANT_DEV_SPEED, ANT_DEV_SPD_CAD, ANT_DEV_POWER,
    ANT_DEV_FE, ANT_DEV_RADAR, ANT_DEV_SHIFTING, ANT_DEV_LIGHT,
};

size_t sensor_list_load(sensor_entry_t *out, size_t max)
{
    FILE *f = fopen(SD_MOUNT_POINT "/CONFIG/sensor_list.json", "r");
    if (!f) {
        ESP_LOGW(TAG, "no sensor_list.json");
        return 0;
    }
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t n = fread(buf, 1, 4095, f);
    fclose(f);
    buf[n] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "sensor_list.json: parse error");
        return 0;
    }
    size_t count = 0;
    cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(root, "DevInfo")) {
        int conn = cJSON_GetNumberValue(cJSON_GetObjectItem(dev, "ConnType"));
        int type = cJSON_GetNumberValue(cJSON_GetObjectItem(dev, "DevType"));
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(dev, "DevID"));
        int sw = cJSON_GetNumberValue(cJSON_GetObjectItem(dev, "Switch"));
        if (!id) continue;
        if (conn != 0) {
            ESP_LOGI(TAG, "skip BLE device %s", id);
            continue;
        }
        if (type < 0 || type >= (int)(sizeof k_vendor_type_to_ant) || !sw || count >= max) continue;
        unsigned num = 0, trans = 1;
        sscanf(id, "%u-%u", &num, &trans);
        out[count].ant_dev_type = k_vendor_type_to_ant[type];
        out[count].dev_num = num;
        out[count].trans_type = trans;
        strncpy(out[count].name, id, sizeof out[count].name - 1);
        out[count].name[sizeof out[count].name - 1] = 0;
        ESP_LOGI(TAG, "paired ANT %s %u-%u", ant_dev_name(out[count].ant_dev_type), num, trans);
        count++;
    }
    cJSON_Delete(root);
    return count;
}
