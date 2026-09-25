/*
 * ANT+ shifting sensors (device type 34): SRAM AXS / eTap, Magene QED,
 * Shimano in its ANT+ shifting mode and anything else that follows the
 * profile.
 *
 * Only two pages matter for the gear display: data page 1 carries the
 * current gear and the drivetrain's gear counts, common page 82 the
 * batteries. The vendor firmware does exactly the same (`ant_shft.c`,
 * page-1 decoder `FUN_422bddcc` @ 0x422bddcc and `mg_shft_dis_evt_handler`
 * @ 0x421bfba4 in the C606 image, `FUN_42348f04` / `FUN_42205110` in the
 * C706 one - byte for byte the same code); its extra pages (0xF0/0xF3..0xF5,
 * the QED teeth tables and the Di2 pages of `ant_di2.c`) are proprietary and
 * only provide tooth counts, shifter buttons and shift modes.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "shifting.h"

static const char *TAG = "shift";

/* ANT+ pages we use */
#define PAGE_SHIFT_STATUS  0x01
#define PAGE_BATTERY       0x52   /* common page 82 */

#define GEAR_REAR_INVALID  0x1F   /* 5-bit field */
#define GEAR_FRONT_INVALID 0x07   /* 3-bit field */
#define VOLT_INVALID       0x0F   /* coarse voltage nibble */

#define MAX_BATT   4              /* batteries of one shifting group we track */
#define CHANGE_Q   8

static shifting_t s_st;
static struct { bool used; uint8_t id, status; float v; } s_batt[MAX_BATT];
static shift_change_t s_q[CHANGE_Q];
static uint8_t s_qhead, s_qcount;
static SemaphoreHandle_t s_lock;

static uint32_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_lock); }

void shifting_reset(void)
{
    lock();
    memset(&s_st, 0, sizeof s_st);
    memset(s_batt, 0, sizeof s_batt);
    s_qhead = s_qcount = 0;
    unlock();
}

void shifting_clear_changes(void)
{
    lock();
    s_qhead = s_qcount = 0;
    unlock();
}

static void push_change(const shift_change_t *c)
{
    if (s_qcount == CHANGE_Q) {          /* drop the oldest */
        s_qhead = (s_qhead + 1) % CHANGE_Q;
        s_qcount--;
    }
    s_q[(s_qhead + s_qcount) % CHANGE_Q] = *c;
    s_qcount++;
}

bool shifting_next_change(shift_change_t *out)
{
    lock();
    bool got = s_qcount > 0;
    if (got) {
        *out = s_q[s_qhead];
        s_qhead = (s_qhead + 1) % CHANGE_Q;
        s_qcount--;
    }
    unlock();
    return got;
}

/* Data page 1 - shift system status:
 *   [1]      shift/event count (the vendor keeps it but never uses it)
 *   [3]      bits 0-4 current rear gear, bits 5-7 current front gear (0-based,
 *            all-ones = invalid)
 *   [4]      bits 0-4 total rear gears, bits 5-7 total front gears
 * A gear counts as known only when its total is non-zero and not smaller
 * than the raw index, exactly as the vendor validates it. */
static void decode_status(const uint8_t *p)
{
    uint8_t rear = p[3] & 0x1F, front = p[3] >> 5;
    uint8_t rear_total = p[4] & 0x1F, front_total = p[4] >> 5;
    shift_change_t c = {0};
    bool first = !s_st.valid;   /* the first page is the starting gear, not a shift */

    bool rv = rear != GEAR_REAR_INVALID && rear_total && rear_total >= rear;
    bool fv = front != GEAR_FRONT_INVALID && front_total && front_total >= front;

    if (rv) {
        c.rear_changed = !s_st.rear_valid || s_st.rear != rear + 1;
        s_st.rear = rear + 1;
        s_st.rear_total = rear_total;
    }
    if (fv) {
        c.front_changed = !s_st.front_valid || s_st.front != front + 1;
        s_st.front = front + 1;
        s_st.front_total = front_total;
    }
    s_st.rear_valid = rv;
    s_st.front_valid = fv;
    if (!rv && !fv) return;

    s_st.valid = true;
    if (c.rear_changed || c.front_changed) {
        c.front_valid = fv;
        c.rear_valid = rv;
        c.front = s_st.front;
        c.rear = s_st.rear;
        push_change(&c);
        if (!first) {
            s_st.shifts++;
            ESP_LOGI(TAG, "gear %s%u/%u x %u/%u", fv ? "" : "?", s_st.front, s_st.front_total,
                     s_st.rear, s_st.rear_total);
        }
    }
}

/* Common page 82 - battery status of one component of the group:
 *   [2]      bits 0-3 number of batteries, bits 4-7 battery identifier
 *   [3..5]   cumulative operating time
 *   [6]      fractional battery voltage, 1/256 V
 *   [7]      bits 0-3 coarse voltage (0x0F = none), bits 4-6 status,
 *            bit 7 operating time resolution
 * Several components (derailleurs, shifters) report under their own
 * identifier; we keep them apart and show the worst one. */
static void decode_battery(const uint8_t *p)
{
    uint8_t id = p[2] >> 4, coarse = p[7] & 0x0F, status = (p[7] >> 4) & 0x07;
    float v = coarse == VOLT_INVALID ? 0 : coarse + p[6] / 256.0f;
    int slot = -1;

    for (int i = 0; i < MAX_BATT; i++) {
        if (s_batt[i].used && s_batt[i].id == id) { slot = i; break; }
    }
    for (int i = 0; slot < 0 && i < MAX_BATT; i++) {
        if (!s_batt[i].used) slot = i;
    }
    if (slot < 0) slot = 0;
    s_batt[slot].used = true;
    s_batt[slot].id = id;
    s_batt[slot].status = status;
    s_batt[slot].v = v;

    /* worst component: lowest voltage, or the highest status when no
     * component reports a voltage (status 1 new .. 5 critical) */
    float min_v = 0;
    uint8_t worst = 0;
    for (int i = 0; i < MAX_BATT; i++) {
        if (!s_batt[i].used) continue;
        if (s_batt[i].v > 0 && (min_v == 0 || s_batt[i].v < min_v)) min_v = s_batt[i].v;
        if (s_batt[i].status >= 1 && s_batt[i].status <= 5 && s_batt[i].status > worst) {
            worst = s_batt[i].status;
        }
    }
    s_st.batt_v = min_v;
    s_st.batt_status = worst;
    s_st.batt_valid = min_v > 0 || worst != 0;
}

void shifting_page(const uint8_t *page)
{
    lock();
    /* page numbers above 0x7F are used by the profile, so no toggle-bit mask */
    switch (page[0]) {
    case PAGE_SHIFT_STATUS: decode_status(page); break;
    case PAGE_BATTERY:      decode_battery(page); break;
    default: break;
    }
    s_st.last_page_ms = now_ms();
    unlock();
}

void shifting_get(shifting_t *out)
{
    lock();
    *out = s_st;
    unlock();
}

bool shifting_live(void)
{
    shifting_t s;
    shifting_get(&s);
    return s.valid && now_ms() - s.last_page_ms < SHIFT_STALE_MS;
}

const char *shifting_batt_text(void)
{
    static const char *k_status[] = { "--", "New", "Good", "OK", "Low", "Crit", "Chrg", "--" };
    shifting_t s;
    shifting_get(&s);
    return s.batt_valid ? k_status[s.batt_status & 7] : "--";
}
