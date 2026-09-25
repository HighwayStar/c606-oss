#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Electronic shifting over ANT+ (device type 34): which gear the drivetrain
 * is in, how many gears it has and the shifter/derailleur battery.
 *
 * ant.c hands every page of a shifting channel to shifting_page(); the UI
 * reads snapshots with shifting_get() and the FIT recorder drains the
 * changes with shifting_next_change(). See docs/HARDWARE.md for the page
 * layout (reverse engineered from the vendor's ant_shft module). */

#define SHIFT_STALE_MS 10000   /* no page for this long -> values are stale */

typedef struct {
    bool valid;                  /* at least one usable page 1 arrived */
    bool front_valid, rear_valid;
    uint8_t front, front_total;  /* current gear 1..total, 1 = innermost */
    uint8_t rear, rear_total;
    uint32_t shifts;             /* front + rear changes since the last reset */
    uint32_t last_page_ms;
    /* worst of the shifting group's batteries (ANT+ common page 82) */
    bool batt_valid;
    float batt_v;                /* 0 when the sensor reports no voltage */
    uint8_t batt_status;         /* 0 unknown, 1 new, 2 good, 3 ok, 4 low, 5 critical, 6 charging */
} shifting_t;

/* One gear change, as the FIT recorder writes it. */
typedef struct {
    bool front_changed, rear_changed;
    bool front_valid, rear_valid;
    uint8_t front, rear;
} shift_change_t;

void shifting_reset(void);                 /* forget gears, battery and pending changes */
void shifting_clear_changes(void);         /* drop pending changes only (start of a ride) */
void shifting_page(const uint8_t *page);   /* one 8-byte ANT+ page */
void shifting_get(shifting_t *out);
bool shifting_live(void);                  /* a page arrived less than SHIFT_STALE_MS ago */
bool shifting_next_change(shift_change_t *out);   /* pops the oldest pending change */
const char *shifting_batt_text(void);      /* "New".."Crit", "--" when unknown */
