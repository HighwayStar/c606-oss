/*
 * Electronic shifting: the ANT+ shifting profile (device type 34 - SRAM AXS /
 * eTap, Magene QED, Shimano in its ANT+ shifting mode) and Shimano's private
 * Di2 stream (device type 128, a D-Fly transmitter).
 *
 * ANT+: data page 1 carries the current gear and the drivetrain's gear
 * counts, common page 82 the batteries. Di2: page 0 carries the current
 * gear and the battery percentage, and the gear counts only arrive in
 * page 0x11 after the head unit has asked for the system info.
 *
 * All of it follows the vendor firmware (`ant_shft.c` / `ant_di2.c`): C606
 * V1.711 decoders `FUN_422bddcc` (ANT+ page 1) and `FUN_422bdb6c` /
 * `FUN_422bdbb0` / `FUN_422bdb8c` (Di2 pages 0 / 0x11 / 0x0B), application
 * handlers `mg_shft_dis_evt_handler` @ 0x421bfba4 and `mg_di2_dis_evt_handler`
 * @ 0x421bbd04, request `ReqDi2SysInfo` @ 0x421bc5c8; the C706 image has the
 * same code. Not implemented: the tooth-count tables behind the Di2
 * chainring/cassette codes, shifter buttons and shift modes.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board.h"
#include "nrf_link.h"
#include "ant.h"
#include "shifting.h"

static const char *TAG = "shift";

/* ANT+ shifting pages */
#define PAGE_SHIFT_STATUS  0x01
#define PAGE_BATTERY       0x52   /* common page 82 */

/* Di2 pages */
#define DI2_PAGE_GEARS     0x00
#define DI2_PAGE_TEETH     0x0B   /* chainring / cassette codes */
#define DI2_PAGE_SPEEDS    0x11   /* number of front / rear gears */
#define DI2_DEV_TYPE       0x80
#define DI2_MAX_FRONT      3
#define DI2_MAX_REAR       13
#define DI2_REQ_PERIOD_MS  10000  /* the vendor asks every ~10 s until it knows */

#define GEAR_REAR_INVALID  0x1F   /* 5-bit field */
#define GEAR_FRONT_INVALID 0x07   /* 3-bit field */
#define VOLT_INVALID       0x0F   /* coarse voltage nibble */

#define MAX_BATT   4              /* batteries of one shifting group we track */
#define CHANGE_Q   8

static shifting_t s_st;
static struct { bool used; uint8_t id, status; float v; } s_batt[MAX_BATT];
static shift_change_t s_q[CHANGE_Q];
static uint8_t s_qhead, s_qcount;
static uint32_t s_di2_req_ms;
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

/* Common to both protocols: take the gears of one page (1-based, 0 = that
 * end is unknown), remember them and queue a change when they moved. */
static void update_gears(uint8_t front, uint8_t rear)
{
    shift_change_t c = {0};
    bool first = !s_st.valid;   /* the first page is the starting gear, not a shift */

    if (rear) {
        c.rear_changed = !s_st.rear_valid || s_st.rear != rear;
        s_st.rear = rear;
    }
    if (front) {
        c.front_changed = !s_st.front_valid || s_st.front != front;
        s_st.front = front;
    }
    s_st.rear_valid = rear != 0;
    s_st.front_valid = front != 0;
    if (!rear && !front) return;

    s_st.valid = true;
    if (c.rear_changed || c.front_changed) {
        c.front_valid = front != 0;
        c.rear_valid = rear != 0;
        c.front = s_st.front;
        c.rear = s_st.rear;
        push_change(&c);
        if (!first) {
            s_st.shifts++;
            ESP_LOGI(TAG, "gear %u/%u x %u/%u", s_st.front, s_st.front_total,
                     s_st.rear, s_st.rear_total);
        }
    }
}

/* ---- ANT+ shifting profile (device type 34) ---------------------------- */

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

    bool rv = rear != GEAR_REAR_INVALID && rear_total && rear_total >= rear;
    bool fv = front != GEAR_FRONT_INVALID && front_total && front_total >= front;

    if (rv) s_st.rear_total = rear_total;
    if (fv) s_st.front_total = front_total;
    update_gears(fv ? front + 1 : 0, rv ? rear + 1 : 0);
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

/* ---- Shimano Di2 (device type 128) ------------------------------------- */

/* Page 0 - gears and system battery:
 *   [2] current front gear, [3] current rear gear (both 1-based, 0 unknown),
 *   [4] battery percentage, [5] unused by the vendor
 * Page 0x11 - the drivetrain: [2] number of front gears, [3] number of rear
 * gears ("Front Speed" / "Rear Speed" in the vendor's log). Page 0x0B holds
 * the chainring and cassette codes that index its tooth-count tables, which
 * we do not carry, so it is ignored. */
void shifting_di2_page(const uint8_t *page)
{
    lock();
    s_st.di2 = true;
    switch (page[0]) {
    case DI2_PAGE_GEARS: {
        uint8_t front = page[2], rear = page[3];
        if (front > DI2_MAX_FRONT) front = 0;
        if (rear > DI2_MAX_REAR) rear = 0;
        if (page[4] && page[4] <= 100) {
            s_st.batt_pct = page[4];
            s_st.batt_valid = true;
        }
        update_gears(front, rear);
        break;
    }
    case DI2_PAGE_SPEEDS:
        if (page[2] && page[2] <= DI2_MAX_FRONT) s_st.front_total = page[2];
        if (page[3] && page[3] <= DI2_MAX_REAR) s_st.rear_total = page[3];
        break;
    default:
        break;
    }
    s_st.last_page_ms = now_ms();
    unlock();
}

/* A Di2 only sends its gear counts when asked: the vendor's ReqDi2SysInfo
 * sends the private page 80 08 FF 00 FF FF FF FF every ~10 s until the
 * answers are in. An ANT page is transmitted as an nRF frame of type 2 whose
 * cmd byte is the ANT device type - the same byte incoming pages carry
 * (vendor `FUN_421b92b0`). Called about once a second from the main loop. */
void shifting_tick(void)
{
    static const uint8_t k_req[8] = { 0x80, 0x08, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    uint32_t now = now_ms();
    bool ask;

    if (!ant_nrf_live(ANT_DEV_DI2)) return;   /* never transmit without a Di2 channel */
    lock();
    ask = s_st.di2 && (!s_st.front_total || !s_st.rear_total) &&
          now - s_st.last_page_ms < SHIFT_STALE_MS &&
          (!s_di2_req_ms || now - s_di2_req_ms >= DI2_REQ_PERIOD_MS);
    if (ask) s_di2_req_ms = now;
    unlock();

    if (ask) {
        ESP_LOGI(TAG, "asking the Di2 for its system info");
        nrf_link_send(NRF_TYPE_SET, DI2_DEV_TYPE, k_req, sizeof k_req);
    }
}

/* ---- readers ----------------------------------------------------------- */

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
