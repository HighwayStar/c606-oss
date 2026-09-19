#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Ride history: the FIT files recorded by this firmware
 * (/sdcard/c606oss/<local date-time>.fit) and by the vendor's
 * (/sdcard/FITS/FIT/<unix time>.fit). The start time comes from the file
 * name, so listing does not read the files; opening one goes through
 * route_scan() (fitread.c) for the summary and the track outline. */

#define HISTORY_NAME_MAX 40     /* = ROUTE_NAME_MAX: entries are route names */

typedef struct {
    char name[HISTORY_NAME_MAX];   /* route name (see route_path): "20260918-123456.fit" or "FITS/FIT/1755918022.fit" */
    uint32_t start_unix;           /* ride start, UTC */
    bool vendor;
} history_entry_t;

/* Newest first; the ride being recorded right now is left out. Returns the
 * count (<= max); *total (optional) gets the number of rides found. */
int history_list(history_entry_t *out, int max, int *total);

/* "2026-09-18 12:34" in local time (config time zone). */
void history_label(const history_entry_t *e, char *buf, size_t n);

esp_err_t history_delete(const char *name);
