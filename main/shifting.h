#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Electronic shifting: which gear the drivetrain is in, how many gears it
 * has and the shifter/derailleur battery.
 *
 * Two radio protocols end up here. ANT+ device type 34 is the open shifting
 * profile (SRAM AXS / eTap, Magene QED, Shimano in its ANT+ mode); device
 * type 128 is Shimano's private Di2 stream, which carries the same values in
 * pages of its own and only sends the gear counts when asked.
 *
 * ant.c hands every page of such a channel to shifting_page() /
 * shifting_di2_page(); the UI reads snapshots with shifting_get() and the
 * FIT recorder drains the changes with shifting_next_change(). See
 * docs/HARDWARE.md for the page layouts (reverse engineered from the
 * vendor's ant_shft / ant_di2 modules). */

#define SHIFT_STALE_MS 10000   /* no page for this long -> values are stale */

typedef struct {
    bool valid;                  /* at least one usable page arrived */
    bool front_valid, rear_valid;
    uint8_t front, front_total;  /* current gear 1..total, 1 = innermost; total 0 = unknown */
    uint8_t rear, rear_total;
    uint32_t shifts;             /* front + rear changes since the last reset */
    uint32_t last_page_ms;
    bool di2;                    /* the values come from a Di2 channel */
    /* battery: a Di2 reports a percentage, the ANT+ profile a voltage and a
     * status (common page 82); 0 means the sensor did not report it */
    bool batt_valid;
    uint8_t batt_pct;
    float batt_v;
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
void shifting_page(const uint8_t *page);       /* one 8-byte ANT+ shifting page (type 34) */
void shifting_di2_page(const uint8_t *page);   /* one 8-byte Di2 page (type 128) */
void shifting_tick(void);                  /* ~1 Hz: asks a Di2 for its system info */
void shifting_get(shifting_t *out);
bool shifting_live(void);                  /* a page arrived less than SHIFT_STALE_MS ago */
bool shifting_next_change(shift_change_t *out);   /* pops the oldest pending change */
const char *shifting_batt_text(void);      /* "New".."Crit", "--" when unknown */
