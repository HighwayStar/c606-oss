#pragma once
#include <stdint.h>
#include <stddef.h>

/* One ANT+ entry from the vendor's /sdcard/CONFIG/sensor_list.json */
typedef struct {
    uint8_t ant_dev_type;   /* ANT+ device type (120 = HR, ...) */
    uint16_t dev_num;
    uint8_t trans_type;
    char name[32];
} sensor_entry_t;

/* Parses the vendor file; returns the number of ANT entries stored (BLE
 * entries are skipped). */
size_t sensor_list_load(sensor_entry_t *out, size_t max);
