#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Minimal Garmin FIT encoder (protocol 2.0, little-endian, no compressed
 * timestamps, no developer fields). The caller describes each message type
 * once as a table of (field number, size, base type); a data message is the
 * fields' bytes packed in that order (see the packed structs in tracklog.c).
 * Definition messages are written on first use of a local message number
 * and again whenever the number is reassigned to another table. */

/* FIT base types (profile "base_type" byte) */
#define FIT_ENUM     0x00
#define FIT_SINT8    0x01
#define FIT_UINT8    0x02
#define FIT_SINT16   0x83
#define FIT_UINT16   0x84
#define FIT_SINT32   0x85
#define FIT_UINT32   0x86
#define FIT_STRING   0x07
#define FIT_UINT8Z   0x0A
#define FIT_UINT16Z  0x8B
#define FIT_UINT32Z  0x8C

/* "not present" values per base type */
#define FIT_INV_U8   0xFF
#define FIT_INV_S8   0x7F
#define FIT_INV_U16  0xFFFF
#define FIT_INV_S16  0x7FFF
#define FIT_INV_U32  0xFFFFFFFFu
#define FIT_INV_S32  0x7FFFFFFF
#define FIT_INV_U8Z  0x00
#define FIT_INV_U16Z 0x0000
#define FIT_INV_U32Z 0x00000000u

/* FIT timestamps count seconds from 1989-12-31T00:00:00Z */
#define FIT_EPOCH_UNIX 631065600u
#define FIT_TS(unix_s) ((uint32_t)((unix_s) - FIT_EPOCH_UNIX))

typedef struct {
    uint8_t num;      /* field definition number from the profile */
    uint8_t size;     /* bytes */
    uint8_t type;     /* FIT_* base type */
} fit_field_t;

typedef struct {
    uint16_t global;          /* global message number */
    uint8_t nfields;
    const fit_field_t *fields;
} fit_mesg_t;

#define FIT_LOCAL_MESGS 16

typedef struct {
    FILE *f;
    uint16_t crc;                 /* running CRC of the data section */
    uint32_t data_size;
    const fit_mesg_t *local[FIT_LOCAL_MESGS];   /* definition currently bound to each local number */
} fit_writer_t;

/* Creates the file and writes a placeholder header (fixed at fit_close). */
esp_err_t fit_open(fit_writer_t *w, const char *path);

/* Writes one data message; `payload` is `mesg`'s fields packed in order and
 * `len` must equal the sum of their sizes (checked). */
esp_err_t fit_write(fit_writer_t *w, uint8_t local, const fit_mesg_t *mesg, const void *payload, size_t len);

/* Sum of the field sizes of a message table. */
size_t fit_mesg_size(const fit_mesg_t *mesg);

/* fflush + fsync: what is on the card can be recovered (missing header size / CRC). */
void fit_sync(fit_writer_t *w);

/* Appends the CRC, patches the header with the data size and closes the file. */
esp_err_t fit_close(fit_writer_t *w);

/* CRC-16 as specified by the FIT protocol (reflected 0x8005 nibble table). */
uint16_t fit_crc(uint16_t crc, const void *data, size_t len);
