/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the Wi-SUN FAN 1.1 LFN unicast mechanics in the FHSS (GS8
 * stage 2, docs/LFN_MAC.md): the stack's real FHSS (rcp/fhss/) on a fake
 * port and a fake MAC, with a virtual µs clock that starts near the 32-bit
 * wrap, so every run crosses it.
 *
 *   1. Slot arithmetic (fhss_ws_lfn.c): anchors from LUTT, next slot, slot at
 *      a time, across the 32-bit wrap; invalid inputs.
 *   2. LFN role: windows open at slot start - guard on the channel of the
 *      node's channel function for the slot, close after the window; FFN
 *      hopping and the periodic TX poll stop; a deferred close is retried;
 *      LUTT-IE written at TX time; stopping the schedule restores hopping.
 *   3. Parent role: the LUTT written by the LFN, read back as the parent
 *      would, gives a TX time inside the LFN's window and the same channel;
 *      frames wait (with a queue poll before the window), are released, and
 *      expire after their TTL; an FFN neighbour and a disabled parent role
 *      give NOT_LFN.
 *   4. LFN broadcast schedule (GS8 stage 4): the parent's slots, frames held
 *      for them, their channel, the LBT-IE and the LTO-IE conversion; the
 *      LFN's sync from LBT-IEs with a parent clock 30 ppm off: rate, phase,
 *      broadcast windows on the parent's channels, widened for drift; its
 *      own unicast slots follow the parent's time base; sync-only listening.
 *
 *   tests/fhss_lfn/run.sh [build dir]
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_config.h"
#include "nanostack/mac/fhss_ws_extension.h"
#include "nanostack/mac/net_fhss.h"
#include "fhss/fhss_ws_lfn.h"
#include "fhss/fhss.h"            // white box: the LFN state (test_sync_over_uc mode 5)
#include "fhss/fhss_common.h"
#include "fhss/fhss_ws.h"
#include "fhss/channel_functions.h"

static uint64_t now_us;
#define CLOCK32_OFFSET_US (UINT32_MAX - 30000000u)   // wraps 30 s into the run
static uint32_t ts(void) { return (uint32_t)(now_us + CLOCK32_OFFSET_US); }
static unsigned checks, failures;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s (t=%" PRIu64 " us)\n", __FILE__, __LINE__, #cond, now_us); \
        } \
    } while (0)

/* ----------------------------------------------------------- fake port */

void platform_enter_critical(void) { }
void platform_exit_critical(void) { }
void mbed_tracef(uint8_t dlevel, const char *grp, const char *fmt, ...) { (void)dlevel; (void)grp; (void)fmt; }
char *mbed_trace_array(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return ""; }
char *trace_array(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return ""; }
uint16_t rand_get_random_in_range(uint16_t min, uint16_t max) { return min + (max - min) / 2; }

// Event loop callback timers: only the FHSS's TX poll timer uses one
static bool poll_timer_armed;
int eventOS_callback_timer_register(void (*fn)(int, uint16_t)) { (void)fn; return 1; }
int eventOS_callback_timer_start(int id, uint16_t slots) { (void)id; poll_timer_armed = slots != 0; return 0; }
int eventOS_callback_timer_stop(int id) { (void)id; poll_timer_armed = false; return 0; }

// FHSS timers: one per callback, absolute deadlines
#define FHSS_TIMERS 4
static struct {
    void (*fn)(const fhss_api_t *, uint16_t);
    bool armed;
    uint64_t deadline;
} ftimers[FHSS_TIMERS];
static const fhss_api_t *ftimer_api;

static int ft_start(uint32_t us, void (*fn)(const fhss_api_t *, uint16_t), const fhss_api_t *api)
{
    int i;
    for (i = 0; i < FHSS_TIMERS && ftimers[i].fn && ftimers[i].fn != fn; i++);
    if (i == FHSS_TIMERS) {
        fprintf(stderr, "too many FHSS timer callbacks\n");
        exit(1);
    }
    ftimers[i].fn = fn;
    ftimers[i].armed = true;
    ftimers[i].deadline = now_us + us;
    ftimer_api = api;
    return 0;
}

static int ft_stop(void (*fn)(const fhss_api_t *, uint16_t), const fhss_api_t *api)
{
    (void)api;
    for (int i = 0; i < FHSS_TIMERS; i++) {
        if (ftimers[i].fn == fn) {
            ftimers[i].armed = false;
        }
    }
    return 0;
}

static uint32_t ft_remaining(void (*fn)(const fhss_api_t *, uint16_t), const fhss_api_t *api)
{
    (void)api;
    for (int i = 0; i < FHSS_TIMERS; i++) {
        if (ftimers[i].fn == fn && ftimers[i].armed && ftimers[i].deadline > now_us) {
            return ftimers[i].deadline - now_us;
        }
    }
    return 0;
}

static uint32_t ft_timestamp(const fhss_api_t *api) { (void)api; return ts(); }

static const fhss_timer_t fake_fhss_timer = {
    .fhss_timer_start = ft_start,
    .fhss_timer_stop = ft_stop,
    .fhss_get_remaining_slots = ft_remaining,
    .fhss_get_timestamp = ft_timestamp,
    .fhss_resolution_divider = 1,
};

static int armed_fhss_timers(void)
{
    int n = 0;
    for (int i = 0; i < FHSS_TIMERS; i++) {
        n += ftimers[i].fn && ftimers[i].armed;
    }
    return n;
}

// Run the earliest FHSS timer due at or before 'until'; false if none
static bool step(uint64_t until)
{
    int best = -1;
    for (int i = 0; i < FHSS_TIMERS; i++) {
        if (ftimers[i].fn && ftimers[i].armed && ftimers[i].deadline <= until &&
                (best < 0 || ftimers[i].deadline < ftimers[best].deadline)) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    if (ftimers[best].deadline > now_us) {
        now_us = ftimers[best].deadline;
    }
    ftimers[best].armed = false;
    ftimers[best].fn(ftimer_api, 0);
    return true;
}

/* ------------------------------------------------------------ fake MAC */

static const uint8_t lfn_mac[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x2a };
static const uint8_t parent_mac[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x00 };
static const uint8_t *own_mac;
static int mac_channel = -1;
static uint16_t queue_size;
static unsigned tx_polls;
static int close_defer;          // closes to defer before accepting

#define EV_MAX 4096
static struct ev {
    uint64_t t;
    fhss_rx_window_e e;
    int channel;
    int ret;
} evs[EV_MAX];
static int nevs;

static uint16_t m_queue_size(const fhss_api_t *a, bool bc) { (void)a; return bc ? 0 : queue_size; }
static int m_mac_address(const fhss_api_t *a, uint8_t *mac) { (void)a; memcpy(mac, own_mac, 8); return 0; }
static uint32_t m_datarate(const fhss_api_t *a) { (void)a; return 150000; }
static uint32_t m_timestamp(const fhss_api_t *a) { (void)a; return ts(); }
static int m_change_channel(const fhss_api_t *a, uint8_t ch) { (void)a; mac_channel = ch; return 0; }
static int m_tx_poll(const fhss_api_t *a) { (void)a; tx_polls++; return 0; }
static int m_bc_notify(const fhss_api_t *a, uint32_t t) { (void)a; (void)t; return 0; }
static int m_coord(const fhss_api_t *a, uint8_t *mac) { (void)a; (void)mac; return 0; }

static int m_rx_window(const fhss_api_t *a, fhss_rx_window_e e, uint8_t ch)
{
    (void)a;
    int ret = 0;
    if (e == FHSS_RX_WINDOW_CLOSE && close_defer > 0) {
        close_defer--;
        ret = 1;
    }
    if (nevs < EV_MAX) {
        evs[nevs++] = (struct ev) { now_us, e, e == FHSS_RX_WINDOW_OPEN ? ch : -1, ret };
    }
    if (e == FHSS_RX_WINDOW_OPEN) {
        mac_channel = ch;
    }
    return ret;
}

static fhss_callback_t mac_callbacks = {
    .read_tx_queue_size = m_queue_size,
    .read_mac_address = m_mac_address,
    .read_datarate = m_datarate,
    .read_timestamp = m_timestamp,
    .change_channel = m_change_channel,
    .tx_poll = m_tx_poll,
    .broadcast_notify = m_bc_notify,
    .read_coord_mac_address = m_coord,
    .read_synch_info = NULL,
    .rx_window = m_rx_window,
};

/* Neighbour table: the LFN (parent role), the parent (LFN role) */
static fhss_ws_neighbor_timing_info_t lfn_neighbor, parent_neighbor;
static bool lfn_neighbor_known, parent_neighbor_known;
static fhss_ws_neighbor_timing_info_t *get_neighbor(const fhss_api_t *a, uint8_t eui64[8])
{
    (void)a;
    if (lfn_neighbor_known && !memcmp(eui64, lfn_mac, 8)) {
        return &lfn_neighbor;
    }
    if (parent_neighbor_known && !memcmp(eui64, parent_mac, 8)) {
        return &parent_neighbor;
    }
    return NULL;
}

/* -------------------------------------------------------------- helpers */

// EU-like plan: 35 channels, all allowed
static void fhss_config(fhss_ws_configuration_t *c, uint8_t uc_dwell, uint32_t bc_interval, uint8_t bc_dwell)
{
    memset(c, 0, sizeof(*c));
    c->ws_uc_channel_function = WS_DH1CF;
    c->ws_bc_channel_function = WS_DH1CF;
    c->bsi = 0x1234;
    c->fhss_uc_dwell_interval = uc_dwell;
    c->fhss_broadcast_interval = bc_interval;
    c->fhss_bc_dwell_interval = bc_dwell;
    c->channel_mask_size = 35;
    for (int i = 0; i < 35; i++) {
        c->channel_mask[i / 32] |= 1u << (i % 32);
        c->unicast_channel_mask[i / 32] |= 1u << (i % 32);
    }
}

static int dh1cf_channel(uint16_t slot, const uint8_t mac[8])
{
    return dh1cf_get_uc_channel_index(slot, (uint8_t *)mac, 35);  // all channels allowed: index = channel
}

static void reset_fakes(void)
{
    memset(ftimers, 0, sizeof(ftimers));
    nevs = 0;
    queue_size = 0;
    tx_polls = 0;
    close_defer = 0;
    mac_channel = -1;
    poll_timer_armed = false;
}

/* ---------------------------------------------------- 1. slot arithmetic */

static void test_sched(void)
{
    fhss_lfn_sched_t s;
    uint16_t slot;
    uint64_t off;

    CHECK(!fhss_lfn_sched_from_lutt(&s, 0, 0, 0, 0));
    CHECK(!fhss_lfn_sched_from_lutt(&s, 1000, 0, 1000, 0));          // offset outside the interval
    CHECK(!fhss_lfn_sched_from_lutt(&s, 16777216, 0, 0, 0));         // above 2^24 - 1 ms (Wi-SUN maximum)
    CHECK(fhss_lfn_sched_from_lutt(&s, 16777215, 0, 0, 0));

    // LUTT: slot 65534, 300 ms into it, received 1 s before the 32-bit wrap
    uint32_t rx = UINT32_MAX - 999999u;
    CHECK(fhss_lfn_sched_from_lutt(&s, 1000, 65534, 300, rx));
    CHECK(s.anchor_us == rx - 300000u);
    CHECK(fhss_lfn_sched_slot_at(&s, rx, &off) == 65534 && off == 300000);
    // 700 ms later the next slot starts, after the wrap of nothing yet
    CHECK(fhss_lfn_sched_next(&s, rx + 1, &slot) == rx + 700000u && slot == 65535);
    CHECK(fhss_lfn_sched_next(&s, rx + 700000u, &slot) == rx + 700000u && slot == 65535);
    // Across the 32-bit wrap (rx + 1 s = 0) and the 16-bit slot wrap
    CHECK(fhss_lfn_sched_next(&s, rx + 700001u, &slot) == rx + 1700000u && slot == 0);
    CHECK(fhss_lfn_sched_slot_at(&s, rx + 1700000u + 5u, &off) == 0 && off == 5);
    CHECK(fhss_lfn_sched_slot_at(&s, rx + 2699999u, &off) == 0 && off == 999999);
    CHECK(fhss_lfn_sched_slot_at(&s, rx + 2700000u, &off) == 1 && off == 0);
    // Before the anchor: the slots before it
    CHECK(fhss_lfn_sched_next(&s, s.anchor_us - 10, &slot) == s.anchor_us && slot == 65534);
    CHECK(fhss_lfn_sched_slot_at(&s, s.anchor_us - 10, &off) == 65533 && off == 1000000 - 10);
    CHECK(fhss_lfn_sched_next(&s, s.anchor_us - 2500000u, &slot) == s.anchor_us - 2000000u && slot == 65532);
    CHECK(fhss_lfn_sched_slot_at(&s, s.anchor_us - 2000000u, &off) == 65532 && off == 0);
    // Re-anchoring keeps the same schedule
    {
        fhss_lfn_sched_t r = s;
        fhss_lfn_sched_reanchor(&r, s.anchor_us + 5300000u);
        CHECK(r.anchor_us == s.anchor_us + 5000000u && r.anchor_slot == 3);
        CHECK(fhss_lfn_sched_next(&r, s.anchor_us + 1, &slot) == s.anchor_us + 1000000u && slot == 65535);
    }
    // Far from the anchor (30 min, interval 7 s)
    CHECK(fhss_lfn_sched_from_lutt(&s, 7000, 10, 0, 1000));
    CHECK(fhss_lfn_sched_next(&s, 1000 + 1800000000u, &slot) == 1000 + 1806000000u && slot == 10 + 258);
    // LTO-IE: the slot that contains the reference time and those after it
    // move by the offset; the interval changes; numbering continues
    {
        fhss_lfn_sched_t l;
        CHECK(fhss_lfn_sched_from_lutt(&l, 5000, 100, 0, 1000000u));
        fhss_lfn_sched_lto(&l, 1000000u + 7000000u, 1200000u, 6000000u);   // in slot 101
        CHECK(l.anchor_us == 6000000u + 1200000u && l.anchor_slot == 101 && l.interval_us == 6000000u);
        CHECK(fhss_lfn_sched_next(&l, 1000000u + 7000000u, &slot) == 13200000u && slot == 102);
        CHECK(fhss_lfn_sched_next(&l, 6500000u, &slot) == 7200000u && slot == 101);     // slot 101 moved later
        fhss_lfn_sched_lto(&l, l.anchor_us, 0, 0);                          // offset 0, same interval: same grid
        CHECK(l.anchor_us == 7200000u && l.anchor_slot == 101 && l.interval_us == 6000000u);
    }
    // Broadcast dwell overlap: intervals of 1000 ms starting at 0 + k, dwell 250 ms
    {
        uint32_t r = 0;
        const uint32_t I = 1000000u, D = 250000u, base = 0xfff00000u;     // across the 32-bit wrap
        CHECK(fhss_lfn_dwell_overlap(base, I, D, base + 3 * I + 100000u, 10000u, &r) && r == base + 3 * I + D);   // in the dwell
        CHECK(!fhss_lfn_dwell_overlap(base, I, D, base + 3 * I + 250000u, 10000u, &r));                           // just after it
        CHECK(!fhss_lfn_dwell_overlap(base, I, D, base + 3 * I + 990000u, 10000u, &r));                           // ends at the next dwell
        CHECK(fhss_lfn_dwell_overlap(base, I, D, base + 3 * I + 990001u, 10000u, &r) && r == base + 4 * I + D);   // runs into it
        CHECK(fhss_lfn_dwell_overlap(base + 5 * I, I, D, base + 3 * I + 10u, 10000u, &r) && r == base + 3 * I + D); // reference after t
        CHECK(!fhss_lfn_dwell_overlap(base, I, D, base + 100u, I - D + 1, &r));                                   // never fits: not blocked
        CHECK(!fhss_lfn_dwell_overlap(base, I, 0, base + 100u, 10u, &r));                                         // no dwell
    }

}

/* ----------------------------------------------------------- 2. LFN role */

static fhss_api_t *create_fhss(const uint8_t *mac, uint8_t uc_dwell, uint32_t bc_interval, uint8_t bc_dwell)
{
    fhss_ws_configuration_t c;
    fhss_api_t *api;

    own_mac = mac;
    fhss_config(&c, uc_dwell, bc_interval, bc_dwell);
    api = ns_fhss_ws_create(&c, &fake_fhss_timer);
    CHECK(api != NULL);
    CHECK(api->lfn_tx_check != NULL);
    api->init_callbacks(api, &mac_callbacks);
    return api;
}

// Byte layout of a WH header IE with the Wi-SUN sub-ID (IEEE 802.15.4 2015:
// length 7 bits, element ID 0x2a, type 0), then the LUTT content
static int write_lutt_ie(uint8_t *buf)
{
    uint16_t hdr = (7 & 0x7f) | (0x2a << 7);   // sub-ID + 6 bytes
    buf[0] = hdr & 0xff;
    buf[1] = hdr >> 8;
    buf[2] = 10;            // WH_IE_LUTT_TYPE
    buf[3] = 0;             // frame type
    memset(buf + 4, 0, 5);
    return 9;
}

static void test_lfn(void)
{
    fhss_ws_lfn_listen_t cfg;
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;
    const uint32_t interval_ms = 1500, window = 4000, guard = 800;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 1020, 255);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    CHECK(cfg.listen_interval_ms == FHSS_LFN_LISTEN_INTERVAL_MS_DEFAULT);
    cfg.listen_interval_ms = interval_ms;
    cfg.rx_window_us = window;
    cfg.rx_guard_us = guard;
    cfg.first_slot_delay_us = 20000;

    // Invalid settings
    fhss_ws_lfn_listen_t bad = cfg;
    bad.rx_guard_us = bad.rx_window_us;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &bad) == -2);
    bad = cfg;
    bad.listen_interval_ms = 0;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &bad) == -2);
    bad = cfg;
    bad.first_slot_delay_us = guard;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &bad) == -2);

    // Configured before the MAC starts: nothing runs yet
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    CHECK(nevs == 0 && armed_fhss_timers() == 0);
    {
        fhss_ws_lfn_parent_t p;
        ns_fhss_ws_lfn_parent_defaults(&p);
        CHECK(ns_fhss_ws_lfn_parent_set(api, &p) == -3);    // not both roles
    }

    // MLME-START: the listen schedule, not the FFN schedules
    uint64_t t_start = now_us;
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    CHECK(nevs == 1 && evs[0].e == FHSS_RX_WINDOWS_START);
    CHECK(armed_fhss_timers() == 1);         // the LFN timer only: no hopping, no broadcast schedule
    CHECK(!poll_timer_armed);                // no periodic TX queue poll
    // ns_fhss_ws_set_parent() is ignored by an LFN
    broadcast_timing_info_t bt = { .broadcast_channel_function = WS_DH1CF, .broadcast_dwell_interval = 255, .broadcast_interval = 1020 };
    CHECK(ns_fhss_ws_set_parent(api, parent_mac, &bt, true) == 0);
    CHECK(armed_fhss_timers() == 1);

    // 200 slots (5 min): open and close of every slot at the expected times
    uint64_t slot0 = t_start + 20000;
    int opens = 0, closes = 0;
    while (step(slot0 + 200ull * interval_ms * 1000 - guard - 1)) {
    }
    for (int i = 1; i < nevs; i++) {
        const struct ev *e = &evs[i];
        if (e->e == FHSS_RX_WINDOW_OPEN) {
            uint64_t start = slot0 + (uint64_t)opens * interval_ms * 1000;
            CHECK(e->t == start - guard);
            CHECK(e->channel == dh1cf_channel((uint16_t)opens, lfn_mac));
            opens++;
        } else if (e->e == FHSS_RX_WINDOW_CLOSE) {
            uint64_t start = slot0 + (uint64_t)closes * interval_ms * 1000;
            CHECK(e->t == start - guard + window);
            closes++;
        } else {
            CHECK(0);
        }
    }
    CHECK(opens == 200 && closes == 200);
    CHECK(armed_fhss_timers() == 1 && !poll_timer_armed);
    // The channel used across slots varies (hopping per slot)
    {
        bool differs = false;
        for (int i = 2; i < nevs; i += 2) {
            differs |= evs[i].channel != evs[1].channel;
        }
        CHECK(differs);
    }

    // A deferred close (frame being received): retried every close_retry_us
    nevs = 0;
    step(UINT64_MAX);          // open slot 200
    CHECK(nevs == 1 && evs[0].e == FHSS_RX_WINDOW_OPEN);
    close_defer = 2;
    uint64_t close_at = slot0 + 200ull * interval_ms * 1000 - guard + window;
    step(UINT64_MAX);
    step(UINT64_MAX);
    step(UINT64_MAX);
    CHECK(nevs == 4 && evs[1].ret == 1 && evs[2].ret == 1 && evs[3].ret == 0);
    CHECK(evs[1].t == close_at && evs[2].t == close_at + cfg.close_retry_us && evs[3].t == close_at + 2 * cfg.close_retry_us);
    step(UINT64_MAX);          // the next window is still on time
    CHECK(nevs == 5 && evs[4].e == FHSS_RX_WINDOW_OPEN && evs[4].t == slot0 + 201ull * interval_ms * 1000 - guard);
    ns_fhss_ws_lfn_stats_get(api, &st);
    CHECK(st.windows_opened == 202 && st.closes_deferred == 1 && st.windows_skipped == 0);

    // LUTT-IE at TX time: slot and offset (ms, rounded) of the TX time
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint32_t tx = (uint32_t)(slot0 + 201ull * interval_ms * 1000 + 734600) + CLOCK32_OFFSET_US;
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, tx);
    CHECK((ie[4] | ie[5] << 8) == 201);
    CHECK((ie[6] | ie[7] << 8 | ie[8] << 16) == 735);

    // A timer that fires late (the node was busy): slots whose window passed
    // are skipped, the schedule keeps its phase
    step(UINT64_MAX);           // close slot 201
    nevs = 0;
    now_us = slot0 + 205ull * interval_ms * 1000 + 100;      // 4 slots late
    for (int i = 0; i < FHSS_TIMERS; i++) {
        if (ftimers[i].armed) {
            ftimers[i].deadline = now_us;
        }
    }
    step(UINT64_MAX);           // late open of slot 202
    step(UINT64_MAX);           // its close, at once
    step(UINT64_MAX);           // open of slot 206, on time
    CHECK(nevs == 3 && evs[2].e == FHSS_RX_WINDOW_OPEN && evs[2].t == slot0 + 206ull * interval_ms * 1000 - guard);
    CHECK(evs[2].channel == dh1cf_channel(206, lfn_mac));
    ns_fhss_ws_lfn_stats_get(api, &st);
    CHECK(st.windows_skipped == 3);

    // The parent's broadcast schedule: an uplink does not start in, or run
    // into, its broadcast dwell; the TX queue is polled at the dwell's end
    {
        broadcast_timing_info_t pbt = { .broadcast_channel_function = WS_DH1CF, .broadcast_dwell_interval = 200,
                                        .broadcast_interval = 1000, .broadcast_interval_offset = 100, .bt_rx_timestamp = ts() };
        // the parent's interval started 100 ms ago: in its dwell for 100 ms more
        CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, false) == 0);
        poll_timer_armed = false;
        CHECK(!api->check_tx_conditions(api, false, 1, FHSS_DATA_FRAME, 100, 0, 0));
        CHECK(poll_timer_armed);
        CHECK(api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts()) == -3);
        CHECK(api->check_tx_conditions(api, false, 1, FHSS_DATA_FRAME, 100, 0, 0) == false);
        poll_timer_armed = false;
        CHECK(api->check_tx_conditions(api, true, 1, FHSS_DATA_FRAME, 100, 0, 0));   // broadcasts are not held
        CHECK(!poll_timer_armed);
        ns_fhss_ws_lfn_stats_get(api, &st);
        CHECK(st.uplink_deferred == 3);
        pbt.bt_rx_timestamp = ts() - 300000u;                                        // 400 ms into its interval
        CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, false) == 0);
        CHECK(api->check_tx_conditions(api, false, 1, FHSS_DATA_FRAME, 100, 0, 0));
        pbt.bt_rx_timestamp = ts() + 104000u;                                         // 4 ms before the next dwell
        CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, false) == 0);
        CHECK(!api->check_tx_conditions(api, false, 1, FHSS_DATA_FRAME, 100, 0, 0)); // airtime + guard runs into it
        pbt.broadcast_dwell_interval = 0;                                            // no schedule: nothing held
        CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, false) == 0);
        CHECK(api->check_tx_conditions(api, false, 1, FHSS_DATA_FRAME, 100, 0, 0));
    }

    // LTO-IE / interval change (ns_fhss_ws_lfn_timing_set): outside a window
    // the next window follows the new schedule at once; inside one, after
    // its close
    {
        step(UINT64_MAX);        // close slot 206
        nevs = 0;
        uint64_t s207 = slot0 + 207ull * interval_ms * 1000;
        // received at the start of slot 206 (its window): the grid moves 300 ms
        // later, so slot 206 comes again at its start + 300 ms, then every 2 s
        CHECK(ns_fhss_ws_lfn_timing_set(api, 300, 2000, (uint32_t)(s207 - interval_ms * 1000) + CLOCK32_OFFSET_US) == 0);
        step(UINT64_MAX);        // open
        CHECK(nevs == 1 && evs[0].e == FHSS_RX_WINDOW_OPEN && evs[0].t == s207 - interval_ms * 1000 + 300000 - guard);
        CHECK(evs[0].channel == dh1cf_channel(206, lfn_mac));
        uint64_t s = evs[0].t + guard;           // slot 206's new start
        // inside the window: the window stays, the next follows the change
        CHECK(ns_fhss_ws_lfn_timing_set(api, 0, 1000, (uint32_t)now_us + CLOCK32_OFFSET_US) == 0);
        step(UINT64_MAX);        // close, on time
        CHECK(nevs == 2 && evs[1].e == FHSS_RX_WINDOW_CLOSE && evs[1].t == s - guard + window);
        step(UINT64_MAX);
        CHECK(nevs == 3 && evs[2].e == FHSS_RX_WINDOW_OPEN && evs[2].t == s + 1000000 - guard);
        CHECK(evs[2].channel == dh1cf_channel(207, lfn_mac));
        CHECK(ns_fhss_ws_lfn_timing_set(api, 0, 16777216, 0) == -2);
    }

    // Stop: the window closes, FFN unicast hopping is back
    nevs = 0;
    CHECK(ns_fhss_ws_lfn_listen_set(api, NULL) == 0);
    CHECK(nevs == 2 && evs[0].e == FHSS_RX_WINDOW_CLOSE && evs[1].e == FHSS_RX_WINDOWS_STOP);
    CHECK(armed_fhss_timers() == 1);         // the unicast hop timer
    CHECK(ns_fhss_delete(api) == 0);
}

/* -------------------------------------------------------- 3. Parent role */

// Run the LFN alone to get a LUTT from it at a given time (the frame the
// parent would receive), and its window for a later slot
struct lfn_view {
    uint16_t lutt_slot;
    uint32_t lutt_offset_ms;
    uint32_t lutt_ts;
    uint64_t slot0;
};

static void test_parent(void)
{
    const uint32_t interval_ms = 2000, window = 5000, guard = 1000;
    fhss_ws_lfn_listen_t lcfg;
    fhss_ws_lfn_parent_t pcfg;
    fhss_ws_lfn_stats_t st;
    struct lfn_view v;
    fhss_api_t *api;
    uint32_t tx_time;

    // The LFN: its schedule starts now; it sends a frame 4.3712 s later
    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&lcfg);
    lcfg.listen_interval_ms = interval_ms;
    lcfg.rx_window_us = window;
    lcfg.rx_guard_us = guard;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &lcfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    v.slot0 = now_us + lcfg.first_slot_delay_us;
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    v.lutt_ts = ts() + 4371200;
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, v.lutt_ts);
    v.lutt_slot = ie[4] | ie[5] << 8;
    v.lutt_offset_ms = ie[6] | ie[7] << 8 | ie[8] << 16;
    CHECK(ns_fhss_delete(api) == 0);

    // The parent: an FFN with hopping and a broadcast schedule
    reset_fakes();
    api = create_fhss(parent_mac, 255, 1020, 255);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    // Disabled: every destination is NOT_LFN
    lfn_neighbor_known = true;
    memset(&lfn_neighbor, 0, sizeof(lfn_neighbor));
    lfn_neighbor.uc_timing_info.unicast_channel_function = WS_DH1CF;
    lfn_neighbor.uc_timing_info.unicast_number_of_channels = 35;
    lfn_neighbor.uc_channel_list.channel_count = 35;
    for (int i = 0; i < 35; i++) {
        lfn_neighbor.uc_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    lfn_neighbor.lfn_uc_timing_info.listen_interval_ms = interval_ms;
    lfn_neighbor.lfn_uc_timing_info.slot_number = v.lutt_slot;
    lfn_neighbor.lfn_uc_timing_info.interval_offset_ms = v.lutt_offset_ms;
    lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp = v.lutt_ts;
    CHECK(api->lfn_tx_check(api, lfn_mac, ts(), &tx_time) == FHSS_LFN_TX_NOT_LFN);

    ns_fhss_ws_lfn_parent_defaults(&pcfg);
    fhss_ws_lfn_parent_t bad = pcfg;
    bad.tx_min_prep_us = bad.tx_lead_us + 1;
    CHECK(ns_fhss_ws_lfn_parent_set(api, &bad) == -2);
    CHECK(ns_fhss_ws_lfn_parent_set(api, &pcfg) == 0);
    {
        fhss_ws_lfn_listen_t l;
        ns_fhss_ws_lfn_listen_defaults(&l);
        CHECK(ns_fhss_ws_lfn_listen_set(api, &l) == -3);    // not both roles
    }
    // An FFN neighbour: NOT_LFN
    CHECK(api->lfn_tx_check(api, parent_mac, ts(), &tx_time) == FHSS_LFN_TX_NOT_LFN);

    // 10 frames requested at assorted times after the LUTT: each waits, the
    // poll comes tx_lead before the window, then it is released for a TX
    // time inside the LFN's window, on the LFN's channel for that slot
    now_us = v.slot0 + 4371200 + 1000;   // just after the LUTT was received
    for (int k = 0; k < 10; k++) {
        now_us += 333333 + 77777 * k;
        uint32_t queued = ts();
        fhss_lfn_tx_e r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
        if (r == FHSS_LFN_TX_WAIT) {
            queue_size = 1;
            unsigned polls = tx_polls;
            // run the parent's timers (hopping, broadcast, LFN poll) until
            // the LFN poll has asked for the queue
            while (tx_polls == polls && step(UINT64_MAX)) {
            }
            CHECK(tx_polls == polls + 1);
            r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
        }
        CHECK(r == FHSS_LFN_TX_NOW);
        CHECK((int32_t)(tx_time - ts()) >= (int32_t)pcfg.tx_min_prep_us && (int32_t)(tx_time - ts()) <= (int32_t)pcfg.tx_lead_us);
        // In the LFN's window: [slot start - guard, slot start - guard + window)
        uint64_t tx64 = now_us + (uint32_t)(tx_time - ts());
        uint64_t n = (tx64 + guard - v.slot0) / (interval_ms * 1000ull);
        uint64_t start = v.slot0 + n * interval_ms * 1000ull;
        CHECK(tx64 >= start - guard && tx64 < start - guard + window);
        // The channel the parent sends on is the LFN's for that slot
        mac_channel = -1;
        CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
        CHECK(mac_channel == dh1cf_channel((uint16_t)n, lfn_mac));
        // Sent: the MAC's TX done puts the parent back on its own channel
        now_us = tx64 + 10000;
        api->data_tx_done(api, false, true, 1);
        queue_size = 0;
    }
    // A silent LFN: no new LUTT for 45 min (beyond the 32-bit clock's
    // 2^31 µs). A parent that checks at least every 2^30 µs keeps the phase
    // (the FHSS re-anchors the neighbour's LUTT); here at 20 min and 45 min.
    for (int k = 0; k < 2; k++) {
        now_us += (k == 0 ? 20 : 25) * 60 * 1000000ull;
        uint32_t queued = ts();
        fhss_lfn_tx_e r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
        if (r == FHSS_LFN_TX_WAIT) {
            queue_size = 1;
            unsigned polls = tx_polls;
            while (tx_polls == polls && step(UINT64_MAX)) {
            }
            r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
        }
        CHECK(r == FHSS_LFN_TX_NOW);
        uint64_t tx64 = now_us + (uint32_t)(tx_time - ts());
        uint64_t n = (tx64 + guard - v.slot0) / (interval_ms * 1000ull);
        uint64_t start = v.slot0 + n * interval_ms * 1000ull;
        CHECK(tx64 >= start - guard && tx64 < start - guard + window);
        mac_channel = -1;
        CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
        CHECK(mac_channel == dh1cf_channel((uint16_t)n, lfn_mac));
        queue_size = 0;
    }
    CHECK(lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp != v.lutt_ts);   // re-anchored

    // A silent LFN and no downlink for 10 h: the parent's seconds timer
    // refreshes its children (ns_fhss_ws_lfn_neighbor_refresh(), here every
    // 60 s), so a downlink after 1 h, 3 h and 10 h of silence still hits the
    // LFN's window on its channel. Without the refresh the reference passed
    // 2^31 µs after 35.8 min and the slot came out wrong.
    {
        uint64_t silent_from = now_us;
        const uint64_t at[3] = { 3600, 3 * 3600, 10 * 3600 };
        for (int k = 0; k < 3; k++) {
            while (now_us < silent_from + at[k] * 1000000ull) {
                now_us += 60 * 1000000ull;
                CHECK(ns_fhss_ws_lfn_neighbor_refresh(api, lfn_mac) == 0);
                // never older than 2^30 µs + one refresh period
                CHECK((uint32_t)(ts() - lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp) <= 0x40000000u + 60000000u);
            }
            uint32_t queued = ts();
            fhss_lfn_tx_e r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
            if (r == FHSS_LFN_TX_WAIT) {
                queue_size = 1;
                unsigned polls = tx_polls;
                while (tx_polls == polls && step(UINT64_MAX)) {
                }
                r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time);
            }
            CHECK(r == FHSS_LFN_TX_NOW);
            uint64_t tx64 = now_us + (uint32_t)(tx_time - ts());
            uint64_t n = (tx64 + guard - v.slot0) / (interval_ms * 1000ull);
            uint64_t start = v.slot0 + n * interval_ms * 1000ull;
            CHECK(tx64 >= start - guard && tx64 < start - guard + window);
            mac_channel = -1;
            CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
            CHECK(mac_channel == dh1cf_channel((uint16_t)n, lfn_mac));
            queue_size = 0;
        }
        // An unknown neighbour or an FFN one: -1
        CHECK(ns_fhss_ws_lfn_neighbor_refresh(api, parent_mac) == -1);
    }

    // TTL: a frame queued more than tx_ttl_intervals listen intervals ago
    uint32_t old = ts() - (uint32_t)pcfg.tx_ttl_intervals * interval_ms * 1000u - 1;
    CHECK(api->lfn_tx_check(api, lfn_mac, old, &tx_time) == FHSS_LFN_TX_EXPIRED);
    ns_fhss_ws_lfn_stats_get(api, &st);
    CHECK(st.tx_released == 15 && st.tx_expired == 1);

    // An FFN destination still takes the FFN path: its UTT schedule, unknown
    // here, so tx_handle says "unknown neighbour" (-2), as before
    CHECK(api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts()) == -2 ||
          api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts()) == -1);

    // Disabled again: NOT_LFN
    CHECK(ns_fhss_ws_lfn_parent_set(api, NULL) == 0);
    CHECK(api->lfn_tx_check(api, lfn_mac, ts(), &tx_time) == FHSS_LFN_TX_NOT_LFN);
    CHECK(ns_fhss_ws_lfn_neighbor_refresh(api, lfn_mac) == -1);
    CHECK(ns_fhss_delete(api) == 0);
}

// An LFN that runs for 50 min (beyond 2^31 µs): its LUTT-IE stays right
static void test_lfn_long(void)
{
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = 60000;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t slot0 = now_us + cfg.first_slot_delay_us;
    while (step(slot0 + 50ull * 60000000ull)) {
    }
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t t = slot0 + 50ull * 60000000ull + 12345678;
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, (uint32_t)t + CLOCK32_OFFSET_US);
    CHECK((ie[4] | ie[5] << 8) == 50);
    CHECK((ie[6] | ie[7] << 8 | ie[8] << 16) == 12346);
    CHECK(ns_fhss_delete(api) == 0);
}


/* Phase B1: listen intervals beyond the 32-bit clock (Wi-SUN maximum 2^24 - 1
 * ms, about 4.66 h). Slot arithmetic with a long interval; an LFN listening
 * every 4 h for 13 h (windows at the right times and channels, across the
 * 32-bit wrap); its parent, from the LUTT the LFN wrote, refreshed every 60 s
 * without traffic, sends 3 h and 7 h later inside the LFN's windows. */
static void test_long_interval(void)
{
    const uint32_t iv_ms = 4 * 3600 * 1000;      // 4 h
    const uint64_t iv = (uint64_t)iv_ms * 1000;
    fhss_lfn_sched_t s;
    uint16_t slot;
    uint64_t off;

    // arithmetic: anchor at a phase, t within 2^31 of it
    CHECK(fhss_lfn_sched_from_lutt(&s, 16777215, 7, 16000000, 1000u));
    CHECK(s.anchor_phase_us == 16000000000ull && s.anchor_us == 1000u);
    CHECK(fhss_lfn_sched_next_delta(&s, 1000u, &slot) == 16777215000ull - 16000000000ull && slot == 8);
    CHECK(fhss_lfn_sched_slot_at(&s, 1000u + 2000000000u, &off) == 8 && off == 18000000000ull - 16777215000ull);
    fhss_lfn_sched_reanchor(&s, 1000u + 2000000000u);
    CHECK(s.anchor_slot == 8 && s.anchor_phase_us == 18000000000ull - 16777215000ull);
    {
        fhss_lfn_sched_t l = s;
        int64_t d;
        CHECK(fhss_lfn_sched_nearest(&l, l.anchor_us, &d) == 8 && d == (int64_t)l.anchor_phase_us);
        fhss_lfn_sched_shift(&l, 1000);          // 1 ms later
        CHECK(fhss_lfn_sched_slot_at(&l, l.anchor_us, &off) == 8 && off == s.anchor_phase_us - 1000);
    }

    // The LFN
    fhss_ws_lfn_listen_t lcfg;
    fhss_api_t *api;
    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&lcfg);
    lcfg.listen_interval_ms = iv_ms;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &lcfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t slot0 = now_us + lcfg.first_slot_delay_us;
    nevs = 0;
    while (step(slot0 + 3 * iv + 3600000000ull)) {
    }
    int opens = 0;
    for (int i = 0; i < nevs; i++) {
        if (evs[i].e != FHSS_RX_WINDOW_OPEN) {
            continue;
        }
        uint64_t k = (evs[i].t + lcfg.rx_guard_us + 1000000 - slot0) / iv;
        CHECK(evs[i].t == slot0 + k * iv - lcfg.rx_guard_us);
        CHECK(evs[i].channel == dh1cf_channel((uint16_t)k, lfn_mac));
        opens++;
    }
    CHECK(opens == 4);                            // slots 0..3 in 13 h
    // a LUTT written 2.5 h into slot 3
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t t_lutt = slot0 + 3 * iv + 9000000000ull;
    while (step(t_lutt - 1000)) {      // the timer events on the way re-anchor the schedule
    }
    now_us = t_lutt - 1000;
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, (uint32_t)(t_lutt + CLOCK32_OFFSET_US));
    uint16_t lutt_slot = ie[4] | ie[5] << 8;
    uint32_t lutt_off_ms = ie[6] | ie[7] << 8 | ie[8] << 16;
    CHECK(lutt_slot == 3 && lutt_off_ms == 9000000);
    CHECK(ns_fhss_delete(api) == 0);

    // The parent
    fhss_ws_lfn_parent_t pcfg;
    uint32_t tx_time;
    reset_fakes();
    api = create_fhss(parent_mac, 255, 1020, 255);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    ns_fhss_ws_lfn_parent_defaults(&pcfg);
    CHECK(ns_fhss_ws_lfn_parent_set(api, &pcfg) == 0);
    lfn_neighbor_known = true;
    memset(&lfn_neighbor, 0, sizeof(lfn_neighbor));
    lfn_neighbor.uc_timing_info.unicast_channel_function = WS_DH1CF;
    lfn_neighbor.uc_timing_info.unicast_number_of_channels = 35;
    lfn_neighbor.uc_channel_list.channel_count = 35;
    for (int i = 0; i < 35; i++) {
        lfn_neighbor.uc_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    lfn_neighbor.lfn_uc_timing_info.listen_interval_ms = iv_ms;
    lfn_neighbor.lfn_uc_timing_info.slot_number = lutt_slot;
    lfn_neighbor.lfn_uc_timing_info.interval_offset_ms = lutt_off_ms;
    lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp = (uint32_t)(t_lutt + CLOCK32_OFFSET_US);
    now_us = t_lutt + 1000;
    for (int k = 0; k < 2; k++) {
        uint64_t until = now_us + (k == 0 ? 3 : 7) * 3600000000ull;
        while (now_us < until) {
            now_us += 60000000ull;
            CHECK(ns_fhss_ws_lfn_neighbor_refresh(api, lfn_mac) == 0);
        }
        uint32_t queued = ts();
        fhss_lfn_tx_e r;
        unsigned polls = 0;
        // the frame waits; the parent polls on the way, then releases it
        while ((r = api->lfn_tx_check(api, lfn_mac, queued, &tx_time)) == FHSS_LFN_TX_WAIT && polls < 100) {
            queue_size = 1;
            unsigned p0 = tx_polls;
            while (tx_polls == p0 && step(UINT64_MAX)) {
            }
            polls++;
        }
        CHECK(r == FHSS_LFN_TX_NOW);
        uint64_t tx64 = now_us + (uint32_t)(tx_time - ts());
        uint64_t n = (tx64 + lcfg.rx_guard_us - slot0) / iv;
        uint64_t start = slot0 + n * iv;
        CHECK(tx64 >= start - lcfg.rx_guard_us && tx64 < start - lcfg.rx_guard_us + lcfg.rx_window_us);
        mac_channel = -1;
        CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
        CHECK(mac_channel == dh1cf_channel((uint16_t)n, lfn_mac));
        now_us = tx64 + 10000;
        api->data_tx_done(api, false, true, 1);
        queue_size = 0;
    }
    lfn_neighbor_known = false;
    CHECK(ns_fhss_delete(api) == 0);
}

/* ------------------------------------------- 4. LFN broadcast schedule */

static int write_wh_ie(uint8_t *buf, uint8_t sub_id, const uint8_t *content, int len)
{
    uint16_t hdr = ((len + 1) & 0x7f) | (0x2a << 7);
    buf[0] = hdr & 0xff;
    buf[1] = hdr >> 8;
    buf[2] = sub_id;
    memcpy(buf + 3, content, len);
    return 3 + len;
}

static uint32_t rd24(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16; }

static int bc_channel(uint16_t slot, uint16_t bsi)
{
    return dh1cf_get_bc_channel_index(slot, bsi, 35);
}

static void test_bc_parent(void)
{
    const uint32_t bc_ms = 10000, uc_ms = 5000;
    fhss_ws_lfn_parent_t pcfg;
    fhss_ws_lfn_bc_t bc = { .interval_ms = bc_ms, .bsi = 0x55 };
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;
    uint32_t tx_time, start;
    uint16_t slot;

    reset_fakes();
    api = create_fhss(parent_mac, 255, 1020, 255);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    CHECK(ns_fhss_ws_lfn_bc_set(api, &bc) == -1);          // needs the parent role
    ns_fhss_ws_lfn_parent_defaults(&pcfg);
    CHECK(ns_fhss_ws_lfn_parent_set(api, &pcfg) == 0);
    CHECK(api->lfn_tx_check(api, NULL, ts(), &tx_time) == FHSS_LFN_TX_EXPIRED);    // no schedule
    fhss_ws_lfn_bc_t bad = bc;
    bad.interval_ms = 0;
    CHECK(ns_fhss_ws_lfn_bc_set(api, &bad) == -2);
    uint64_t t0 = now_us;
    CHECK(ns_fhss_ws_lfn_bc_set(api, &bc) == 0);
    // Slot 0 starts one interval later
    CHECK(ns_fhss_ws_lfn_bc_next(api, &slot, &start) == 0);
    CHECK(slot == 0 && start == (uint32_t)(t0 + bc_ms * 1000ull + CLOCK32_OFFSET_US));
    // A broadcast frame waits for the slot, polled tx_lead before it
    CHECK(api->lfn_tx_check(api, NULL, ts(), &tx_time) == FHSS_LFN_TX_WAIT);
    while (step(t0 + bc_ms * 1000ull)) {
    }
    CHECK(now_us == t0 + bc_ms * 1000ull - pcfg.tx_lead_us);
    CHECK(tx_polls == 0);   // queue empty in the fake: nothing to poll
    CHECK(api->lfn_tx_check(api, NULL, ts(), &tx_time) == FHSS_LFN_TX_NOW);
    CHECK(tx_time == start + pcfg.tx_offset_us);
    // Its channel: the broadcast channel function of slot 0 with the LFN BSI,
    // whatever the FFN schedule
    CHECK(api->tx_handle(api, true, NULL, FHSS_LFN_BC_FRAME, 50, 4, 2, tx_time) == 0);
    CHECK(mac_channel == bc_channel(0, 0x55));
    CHECK(api->use_broadcast_queue(api, true, FHSS_LFN_BC_FRAME) == false);
    CHECK(api->use_broadcast_queue(api, true, FHSS_DATA_FRAME) == true);
    // LBT-IE at TX time: slot, offset (ms, rounded)
    uint8_t ie[32], zero[8] = { 0 };
    int len = write_wh_ie(ie, 11, zero, 5);
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, tx_time);
    CHECK((ie[3] | ie[4] << 8) == 0 && rd24(ie + 5) == 0);
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, start + 3 * bc_ms * 1000 + 4321600);
    CHECK((ie[3] | ie[4] << 8) == 3 && rd24(ie + 5) == 4322);
    // Long after the start (32-bit clock): still the right slot. The FFN
    // broadcast timer keeps the anchor near.
    uint64_t later = now_us + 3000ull * 1000000;     // 50 min
    while (step(later)) {
    }
    now_us = later;
    CHECK(ns_fhss_ws_lfn_bc_next(api, &slot, &start) == 0);
    CHECK(slot == 300 && start == (uint32_t)(t0 + 301ull * bc_ms * 1000 + CLOCK32_OFFSET_US));

    // LTO-IE to an LFN: the phase against the broadcast slots becomes an
    // offset against the LFN's slot nearest to the frame
    lfn_neighbor_known = true;
    memset(&lfn_neighbor, 0, sizeof(lfn_neighbor));
    lfn_neighbor.uc_timing_info.unicast_channel_function = WS_DH1CF;
    lfn_neighbor.uc_timing_info.unicast_number_of_channels = 35;
    lfn_neighbor.uc_channel_list.channel_count = 35;
    for (int i = 0; i < 35; i++) {
        lfn_neighbor.uc_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    lfn_neighbor.lfn_uc_timing_info.listen_interval_ms = 6000;
    lfn_neighbor.lfn_uc_timing_info.slot_number = 7;
    lfn_neighbor.lfn_uc_timing_info.interval_offset_ms = 1234;
    lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp = ts();
    uint32_t lfn_slot7 = ts() - 1234000;
    // Released for its next window
    uint64_t until = now_us + 20000000;
    fhss_lfn_tx_e r;
    while ((r = api->lfn_tx_check(api, lfn_mac, ts(), &tx_time)) == FHSS_LFN_TX_WAIT && step(until)) {
    }
    CHECK(r == FHSS_LFN_TX_NOW);
    uint8_t lto[6] = { 0 };
    const uint32_t phase_ms = 900;
    lto[0] = phase_ms & 0xff; lto[1] = phase_ms >> 8; lto[2] = 0;
    lto[3] = uc_ms & 0xff; lto[4] = (uc_ms >> 8) & 0xff; lto[5] = uc_ms >> 16;
    len = write_wh_ie(ie, 17, lto, 6);
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, tx_time);
    uint32_t offset_ms = rd24(ie + 3);
    CHECK(offset_ms < uc_ms);
    // The LFN's slot nearest to the frame, moved by the offset, is 900 ms
    // after a broadcast slot start modulo the new interval (5 s divides 10 s)
    uint32_t s_n = lfn_slot7 + (uint32_t)(((uint32_t)(tx_time - lfn_slot7) + 3000000) / 6000000) * 6000000;
    uint32_t moved = s_n + offset_ms * 1000;
    uint32_t bc0 = (uint32_t)(t0 + bc_ms * 1000ull + CLOCK32_OFFSET_US);
    uint32_t phase = (uint32_t)(moved - bc0 - phase_ms * 1000) % (uc_ms * 1000);
    CHECK(phase < 1000 || phase > uc_ms * 1000 - 1000);
    // Applied on the ACK with the offset as written
    CHECK(ns_fhss_ws_lfn_neighbor_lto(api, lfn_mac, phase_ms, uc_ms, tx_time) == 0);
    CHECK(lfn_neighbor.lfn_uc_timing_info.listen_interval_ms == uc_ms);
    // the same grid: the reference is a slot start of it, phase 0
    CHECK(lfn_neighbor.lfn_uc_timing_info.interval_offset_ms == 0 && lfn_neighbor.lfn_uc_timing_info.interval_offset_rem_us == 0 &&
          (int32_t)(lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp - moved) % (int32_t)(uc_ms * 1000) == 0);
    CHECK(ns_fhss_ws_lfn_stats_get(api, &st) == 0 && st.bc_tx_released == 1);
    lfn_neighbor_known = false;
    CHECK(ns_fhss_delete(api) == 0);
}

static void test_bc_lfn(bool sync_only)
{
    const uint32_t bc_ms = 10000, uc_ms = 5000, guard = 1000, bc_window = 5000;
    const int32_t r_ppb = 30000;           // our clock against the parent's
    fhss_ws_lfn_listen_t cfg;
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    CHECK(cfg.drift_ppm == FHSS_LFN_DRIFT_PPM_DEFAULT && cfg.bc_rx_window_us == FHSS_LFN_BC_RX_WINDOW_US_DEFAULT &&
          cfg.drift_floor_ppb == FHSS_LFN_DRIFT_FLOOR_PPB_DEFAULT);
    cfg.listen_interval_ms = uc_ms;
    cfg.rx_guard_us = guard;
    cfg.bc_rx_window_us = bc_window;
    cfg.bc_sync_only = sync_only;
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 5, .channel_function = WS_DH1CF };
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == -1);       // not an LFN yet
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    // The parent's slot k starts at P0 + k x B on its clock, at
    // L0 + k x B x (1 + r) on ours
    uint64_t L0 = now_us + 3000000;
    #define PSTART(k) (L0 + (uint64_t)(k) * bc_ms * 1000 + (uint64_t)(k) * bc_ms * (r_ppb / 1000) / 1000)
    // First sync: an LPA-like frame within slot 3 (offset in ms)
    now_us = PSTART(3) + 4321000;
    sy.slot = 3;
    sy.offset_ms = 4321;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    sy.offset_ms = 10000;
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == -2);
    // Exact syncs (offset 0) at slots 5, 10, 15: the rate is measured from
    // slot 5 on; the phase error falls to the rounding
    int32_t err[3];
    for (int i = 0; i < 3; i++) {
        uint16_t k = 5 + 5 * i;
        while (step(PSTART(k) - 1)) {
        }
        now_us = PSTART(k) + 500;          // received 500 µs into the slot (TX timing)
        sy.slot = k;
        sy.offset_ms = 0;
        sy.rx_timestamp_us = ts();
        CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
        CHECK(ns_fhss_ws_lfn_stats_get(api, &st) == 0);
        err[i] = st.last_sync_error_us;
    }
    CHECK(st.rate_ppb > r_ppb - 200 && st.rate_ppb < r_ppb + 200);
    CHECK(err[1] > 1400 && err[1] < 1600);           // 5 slots at 30 ppm, no rate yet
    CHECK(err[2] > -50 && err[2] < 50);
    // From now on: broadcast windows at the predicted slot starts, widened
    // by drift_floor_ppb per second since the last sync (the rate is known),
    // on the parent's channel
    uint64_t t_sync = now_us - 500;      // the slot start, our clock
    nevs = 0;
    while (step(PSTART(26))) {
    }
    int bc_opens = 0, uc_opens = 0;
    uint64_t prev_uc = 0;
    for (int i = 0; i < nevs; i++) {
        const struct ev *e = &evs[i];
        if (e->e != FHSS_RX_WINDOW_OPEN) {
            continue;
        }
        // broadcast window: near a parent slot start
        int k = (int)((e->t - L0 + 5000000) / (bc_ms * 1000ull));
        int64_t d = (int64_t)PSTART(k) + 500 - (int64_t)e->t;   // slot start (as received) - open time
        if (d > 0 && d < 10000) {
            // the rate is measured: the floor (the drift left after the
            // correction, under 1 ppm here, doubled, is below it)
            uint64_t widening = (PSTART(k) - t_sync) * cfg.drift_floor_ppb / 1000000000;
            CHECK(d > (int64_t)(guard + widening) - 100 && d < (int64_t)(guard + widening) + 100);
            CHECK(e->channel == bc_channel((uint16_t)k, 0x77));
            CHECK(!sync_only || k % 5 == 0);
            bc_opens++;
        } else {
            // unicast window: the parent's interval on our clock
            if (prev_uc) {
                int64_t gap = (int64_t)(e->t - prev_uc);
                int64_t want = (int64_t)uc_ms * 1000 + (int64_t)uc_ms * (r_ppb / 1000) / 1000;
                // the widening grows by drift_ppm x interval between windows
                CHECK(gap > want - (int64_t)cfg.drift_ppm * uc_ms / 1000 - 5 && gap < want + 5);
            }
            prev_uc = e->t;
            uc_opens++;
        }
    }
    if (bc_opens != (sync_only ? 2 : 11)) {
        fprintf(stderr, "bc_opens %d uc_opens %d\n", bc_opens, uc_opens);
        for (int i = 0; i < nevs; i++) {
            if (evs[i].e == FHSS_RX_WINDOW_OPEN) {
                fprintf(stderr, "  open t=%" PRIu64 " ch=%d\n", evs[i].t - L0, evs[i].channel);
            }
        }
    }
    CHECK(bc_opens == (sync_only ? 2 : 11));   // slots 16-26
    CHECK(uc_opens >= 20);
    // Stopped during a broadcast window while the MAC receives a frame (it
    // defers the close): the close is asked again every close_retry_us
    // until done, not left to our next unicast window (before: the radio
    // stayed on a listen interval, hours under Eco)
    nevs = 0;
    bool in_bc = false;
    while (!in_bc && step(PSTART(36))) {
        if (nevs && evs[nevs - 1].e == FHSS_RX_WINDOW_OPEN) {
            int k = (int)((evs[nevs - 1].t - L0 + 5000000) / (bc_ms * 1000ull));
            int64_t d = (int64_t)PSTART(k) + 500 - (int64_t)evs[nevs - 1].t;
            in_bc = d > 0 && d < 10000;
        }
    }
    CHECK(in_bc);
    now_us += 1000;                     // inside the window, a frame arriving
    uint64_t t_stop = now_us;
    close_defer = 2;
    nevs = 0;
    CHECK(ns_fhss_ws_lfn_bc_stop(api) == 0);
    while (step(t_stop + 10 * cfg.close_retry_us)) {
    }
    int closes = 0;
    for (int i = 0; i < nevs; i++) {
        if (evs[i].e == FHSS_RX_WINDOW_CLOSE) {
            CHECK(evs[i].t == t_stop + (uint64_t)closes * cfg.close_retry_us);
            CHECK(evs[i].ret == (closes < 2));
            closes++;
        }
    }
    CHECK(closes == 3);
    CHECK(close_defer == 0);
    // Stop: no more broadcast windows
    CHECK(ns_fhss_ws_lfn_bc_stop(api) == 0);
    nevs = 0;
    while (step(PSTART(40))) {
    }
    for (int i = 0; i < nevs; i++) {
        if (evs[i].e == FHSS_RX_WINDOW_OPEN) {
            int k = (int)((evs[i].t - L0 + 5000000) / (bc_ms * 1000ull));
            int64_t d = (int64_t)PSTART(k) - (int64_t)evs[i].t;
            CHECK(!(d > 0 && d < 10000));
        }
    }
    #undef PSTART
    CHECK(ns_fhss_delete(api) == 0);
}

/* LND discovery (black-box phase 2): discovery slot i is on the LFN's
 * unicast channel of slot number first_slot + i; the parent's LPA at a
 * requested time goes on the LFN's channel of the slot number given */
static void test_discovery(void)
{
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;
    const uint16_t first = 65530;       // wraps past 65535
    const uint32_t slot_us = 60000, nslots = 40;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = 6000;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t start = now_us + 1550000;
    CHECK(ns_fhss_ws_lfn_discovery_set(api, (uint32_t)(start + CLOCK32_OFFSET_US), nslots * slot_us, first, slot_us) == 0);
    nevs = 0;
    while (step(start + nslots * slot_us + 1000)) {
    }
    int opens = 0;
    bool closed = false;
    for (int i = 0; i < nevs; i++) {
        if (evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].t >= start && evs[i].t < start + nslots * slot_us) {
            uint32_t k = (uint32_t)((evs[i].t - start) / slot_us);
            CHECK(evs[i].t == start + k * slot_us);
            CHECK(evs[i].channel == dh1cf_channel((uint16_t)(first + k), lfn_mac));
            opens++;
        } else if (evs[i].e == FHSS_RX_WINDOW_CLOSE && evs[i].t == start + nslots * slot_us) {
            closed = true;
        }
    }
    // one open per slot whose channel differs from the previous slot's
    int expect = 1;
    for (uint32_t k = 1; k < nslots; k++) {
        expect += dh1cf_channel((uint16_t)(first + k), lfn_mac) != dh1cf_channel((uint16_t)(first + k - 1), lfn_mac);
    }
    CHECK(opens == expect);
    CHECK(closed);
    CHECK(ns_fhss_delete(api) == 0);

    // The parent: an LPA at a requested time, channel slot 1234
    fhss_ws_lfn_parent_t pcfg;
    uint32_t tx_time = 0;
    reset_fakes();
    api = create_fhss(parent_mac, 255, 1020, 255);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    lfn_neighbor_known = true;
    memset(&lfn_neighbor, 0, sizeof(lfn_neighbor));
    lfn_neighbor.uc_timing_info.unicast_channel_function = WS_DH1CF;
    lfn_neighbor.uc_timing_info.unicast_number_of_channels = 35;
    lfn_neighbor.uc_channel_list.channel_count = 35;
    for (int i = 0; i < 35; i++) {
        lfn_neighbor.uc_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    lfn_neighbor.lfn_uc_timing_info.listen_interval_ms = 6000;
    lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp = ts();
    ns_fhss_ws_lfn_parent_defaults(&pcfg);
    CHECK(ns_fhss_ws_lfn_parent_set(api, &pcfg) == 0);
    uint32_t req = ts() + 10000;
    CHECK(api->lfn_tx_at(api, lfn_mac, req, 1234, &tx_time) == FHSS_LFN_TX_NOW && tx_time == req);
    // the CCA just before the TX time and the TX itself: the slot's channel
    CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time - 1000) == 0);
    CHECK(mac_channel == dh1cf_channel(1234, lfn_mac));
    mac_channel = -1;
    CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
    CHECK(mac_channel == dh1cf_channel(1234, lfn_mac));
    // without a slot: the LFN's slot nearest to the TX time (slot 0 here)
    CHECK(api->lfn_tx_at(api, lfn_mac, req, -1, &tx_time) == FHSS_LFN_TX_NOW);
    CHECK(api->tx_handle(api, false, (uint8_t *)lfn_mac, FHSS_DATA_FRAME, 100, 0, 0, tx_time) == 0);
    CHECK(mac_channel == dh1cf_channel(0, lfn_mac));
    lfn_neighbor_known = false;
    CHECK(ns_fhss_delete(api) == 0);
}

/* A sync (an LPA) during a discovery window, whose time covers our next
 * unicast slot: our unicast windows go on after the discovery window. The
 * own-slot timer, at that slot during the discovery window, must leave the
 * slot to the sync's schedule and not stop for good. */
static void test_sync_in_discovery(void)
{
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = 6000;
    cfg.first_slot_delay_us = 3000000;
    cfg.bc_sync_only = true;    // no broadcast window in the next 9 min
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    // the discovery window from 1 s to 5 s, over our slot 0 (3 s)
    uint64_t t0 = now_us;
    CHECK(ns_fhss_ws_lfn_discovery_set(api, (uint32_t)(t0 + 1000000 + CLOCK32_OFFSET_US), 4000000, 0, 0) == 0);
    while (step(t0 + 2000000)) {
    }
    now_us = t0 + 2000000;
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = 10000, .bsi = 0x77, .sync_period = 60, .channel_function = WS_DH1CF,
                                 .slot = 3, .offset_ms = 4321, .rx_timestamp_us = ts() };
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    nevs = 0;
    while (step(t0 + 40000000)) {
    }
    int uc_opens = 0;
    for (int i = 0; i < nevs; i++) {
        uc_opens += evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].t > t0 + 5000000 &&
                    evs[i].channel != bc_channel((uint16_t)(3 + (evs[i].t - (t0 + 2000000 - 4321000)) / 10000000), 0x77);
    }
    if (uc_opens < 4) {
        fprintf(stderr, "sync_in_discovery: %d unicast windows after the discovery window\n", uc_opens);
    }
    CHECK(uc_opens >= 4);
    CHECK(ns_fhss_delete(api) == 0);
}

/* Follow-up of phase B1: the LFN's own slots, once the rate is measured,
 * where the parent expects them from its view (the LFN's last LUTT-IE), to
 * well under a ms: 4 h interval, a 10 h sync period (Eco), our clock 15.6 ppm
 * fast against the parent's, LUTT-IEs written between syncs. The parent's
 * view: the LUTT's slot started offset (ms, read on the parent's clock)
 * before the frame, the slots follow every interval on the parent's clock.
 * Before the fix (own slots moved by the scaled phase error, LUTT offset
 * written in our µs) the windows were 0.1-0.4 s off. No widening
 * (drift_ppm 0), so a window opens exactly rx_guard_us before its slot. */
static void test_own_ref(void)
{
    const uint32_t iv_ms = 4 * 3600 * 1000, bc_ms = 600000, guard = 1000;
    const double k = 1.0 + 15600e-9;          // our µs per parent µs
    fhss_ws_lfn_listen_t cfg;
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = iv_ms;
    cfg.rx_guard_us = guard;
    cfg.drift_ppm = 0;
    cfg.bc_sync_only = true;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t L0 = now_us + 3000000;
    #define PS(k_) (L0 + (uint64_t)((double)(k_) * bc_ms * 1000 * k))
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 60, .channel_function = WS_DH1CF };
    // an LPA within broadcast slot 1
    now_us = PS(1) + 4321000;
    sy.slot = 1;
    sy.offset_ms = 4321;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t lutt_t = 0;
    uint16_t lutt_slot = 0;
    uint32_t lutt_off = 0;
    // uplinks at 2 h and 12 h (LUTT-IE), syncs at slots 60 (10 h) and 120 (20 h)
    const uint64_t up[2] = { 2ull * 3600000000ull, 12ull * 3600000000ull };
    const uint16_t syn[2] = { 60, 120 };
    for (int i = 0; i < 2; i++) {
        while (step(L0 + up[i])) {
        }
        now_us = L0 + up[i];
        api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, ts());
        api->data_tx_done(api, false, true, 1);     // acknowledged
        lutt_t = now_us;
        lutt_slot = ie[4] | ie[5] << 8;
        lutt_off = ie[6] | ie[7] << 8 | ie[8] << 16;
        while (step(PS(syn[i]) - 1)) {
        }
        now_us = PS(syn[i]) + 500;
        sy.slot = syn[i];
        sy.offset_ms = 0;
        sy.rx_timestamp_us = ts();
        CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    }
    CHECK(ns_fhss_ws_lfn_stats_get(api, &st) == 0);
    CHECK(st.rate_ppb > 15400 && st.rate_ppb < 15800);
    // The next windows (to 29 h): where the parent's view puts their slots
    nevs = 0;
    while (step(L0 + 29ull * 3600000000ull)) {
    }
    int seen = 0;
    for (int i = 0; i < nevs; i++) {
        if (evs[i].e != FHSS_RX_WINDOW_OPEN || evs[i].t < PS(120) + 1000000) {
            continue;
        }
        double start = (double)evs[i].t + guard;
        // the parent's view of slot n, on our clock
        double s0 = (double)lutt_t - (double)lutt_off * 1000 * k;
        long n = lround((start - s0) / ((double)iv_ms * 1000 * k));
        double view = s0 + (double)n * iv_ms * 1000 * k;
        if (fabs(start - view) > 500) {
            fprintf(stderr, "own window %ld: %.0f us from the parent's view\n", n, start - view);
        }
        CHECK(fabs(start - view) <= 500);
        CHECK(evs[i].channel == dh1cf_channel((uint16_t)(lutt_slot + n), lfn_mac));
        seen++;
    }
    CHECK(seen == 2);       // 24 h and 28 h
    #undef PS
    CHECK(ns_fhss_delete(api) == 0);
}

/* Our slots against the parent's view of them after a long silence, with
 * the timing of real syncs (the kits, 2026-11-04 overnight run): 5 s listen
 * interval, the parent's LFN broadcast every 10 s with a sync frame every
 * 5th slot, the parent's clock 31 ppm off ours, each sync's time off by up
 * to +-40 µs (the parent's TX timing on a 32 kHz tick, our RX timestamp).
 * One uplink (LUTT-IE), then 6 h of silence: in every window the parent's
 * TX at the start of our slot (as its view puts it) must fall inside the
 * window with 1 ms to spare. Before the fix the rate was measured again
 * from a new reference every 2^30 µs, over spans down to one sync period
 * (50 s): 80 µs of jitter there is 1.6 ppm, 6 ms after 4000 s of silence,
 * and the kit LFN missed 20 downlink windows in a row (dl16). */
static uint32_t jit_state;
static int32_t jitter_us(int32_t max)
{
    jit_state = jit_state * 1103515245u + 12345u;
    return (int32_t)((jit_state >> 8) % (uint32_t)(2 * max + 1)) - max;
}

/* lost_until (early view): the uplink 20 s in, and the sync frames lost
 * until that slot (350 s to 10 050 s): the rate is first measured one sync
 * period after the first sync heard, long after the parent's view was set.
 * Our slots must still follow the view through the silence (an LFN that
 * sends only ACKs after its join), from the time the view is placed on the
 * parent's clock for good (the rate measured over the view's distance to
 * the first sync heard; the misses before are printed, not checked). Each
 * case with four jitter sequences. Before the fix the view was placed at
 * the first rate, measured over 50 s (80 µs of jitter: up to 1.6 ppm), and
 * that rate's error over the view's age stayed for the whole silence: 6 of
 * the 32 cases missed every window (-0.1 to -10.5 ms) */
static void test_own_ref_jitter(uint32_t lost_until, uint32_t seed)
{
    bool early_view = lost_until != 0;
    jit_state = seed;
    const uint32_t iv_ms = 5000, bc_ms = 10000, guard = 1000;
    const double k = 1.0 - 31124e-9;          // our µs per parent µs
    const uint64_t silence_us = 6ull * 3600000000ull;
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = iv_ms;
    cfg.rx_guard_us = guard;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t L0 = now_us + 3000000;
    #define PS(k_) (L0 + (uint64_t)((double)(k_) * bc_ms * 1000 * k))
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 5, .channel_function = WS_DH1CF };
    // an LPA within broadcast slot 1
    now_us = PS(1) + 4321000;
    sy.slot = 1;
    sy.offset_ms = 4321;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t lutt_t = 0;
    uint32_t lutt_off = 0;
    uint64_t t_lutt = L0 + (early_view ? 20000000ull : 600000000ull);  // the uplink
    uint64_t t_end = t_lutt + silence_us;
    bool lutt_done = false;
    nevs = 0;
    int seen = 0, bad = 0, early_bad = 0;
    double worst = 1e9;
    /* With the syncs lost, the view is placed on the parent's clock for good
       once the rate is measured over the view's distance to the rate's
       reference (the first sync heard): until then our slots follow it at
       each better rate, and may miss (counted apart) */
    uint32_t settled_slot = 2 * lost_until + 5;
    for (uint32_t s = 5; PS(s) < t_end; s += 5) {
        uint64_t t_sync = PS(s) + 500 + jitter_us(40);
        bool lost = early_view && s < lost_until;
        if (!lutt_done && t_lutt < t_sync) {
            while (step(t_lutt)) {
            }
            now_us = t_lutt;
            api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, ts());
            api->data_tx_done(api, false, true, 1);     // acknowledged
            lutt_t = now_us;
            lutt_off = ie[6] | ie[7] << 8 | ie[8] << 16;
            lutt_done = true;
            nevs = 0;
        }
        while (step(t_sync)) {
        }
        now_us = t_sync;
        sy.slot = (uint16_t)s;
        sy.offset_ms = 0;
        sy.rx_timestamp_us = ts();
        if (!lost) {
            CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
        }
        if (!lutt_done || lost || (early_view && s < lost_until + 10)) {
            // the windows from the first syncs heard on: before the rate
            // is measured (the second one), our slots follow the syncs'
            // phase error, not the view
            nevs = 0;
            continue;
        }
        bool settled = !early_view || s >= settled_slot;
        // the unicast windows since the previous sync: open, close pairs
        for (int i = 0; i + 1 < nevs; i++) {
            if (evs[i].e != FHSS_RX_WINDOW_OPEN || evs[i + 1].e != FHSS_RX_WINDOW_CLOSE) {
                continue;
            }
            double open = (double)evs[i].t, close = (double)evs[i + 1].t;
            double r = fmod(open - (double)L0, bc_ms * 1000 * k);
            if (r < 0.5e6 || r > bc_ms * 1000 * k - 0.5e6) {
                continue;       // a broadcast window (near a parent slot start)
            }
            double s0 = (double)lutt_t - (double)lutt_off * 1000 * k;
            long n = lround((open + guard - s0) / ((double)iv_ms * 1000 * k));
            double view = s0 + (double)n * iv_ms * 1000 * k;
            double margin = fmin(view - open, close - (view + 1000));
            if (!settled) {
                early_bad += margin < 0;
                continue;
            }
            if (margin < worst) {
                worst = margin;
            }
            if (margin < 0 && bad++ < 5) {
                fprintf(stderr, "own window %ld, %.0f s after the uplink: parent TX %.0f us from the window's opening, window %.0f us\n",
                        n, (view - (double)lutt_t) / 1e6, view - open, close - open);
            }
            seen++;
        }
        nevs = 0;
    }
    if (bad || worst < (early_view ? 400 : 700) || getenv("FHSS_LFN_VERBOSE")) {
        fprintf(stderr, "own_ref_jitter (syncs lost before slot %" PRIu32 ", seed %" PRIu32 "): %d of %d windows missed, worst margin %.0f us; %d missed before the view was placed\n",
                lost_until, seed, bad, seen, worst, early_bad);
    }
    CHECK(seen > (early_view ? (int)((t_end - PS(settled_slot)) / (iv_ms * 1000ull)) - 20 : 4000));
    CHECK(bad == 0);
    /* our slot start within 300 µs of the parent's view (the guard is 1 ms);
       with the early view, 600 µs: the view goes on the parent's clock at a
       rate measured over at least its distance to the rate's reference
       sync (about two syncs' timing error) */
    CHECK(worst >= (early_view ? 400 : 700));
    #undef PS
    CHECK(ns_fhss_delete(api) == 0);
}

/* Our Enh-ACKs carry a LUTT-IE, which parents do not take as a view of our
 * slots (ours: ws_llc_data_service.c; Silicon Labs' wsbrd v2.10.3,
 * ws_llc_data_confirm: lifetime only). The timeline of the emulator's Eco
 * silence test (seed 1, LFN under the border router): 300 s listen interval,
 * 600 s LFN broadcast, sync every 60th slot (10 h), the parent's clock
 * 15.6 ppm off ours; a data frame (LUTT-IE, the parent's view) at 167 s,
 * syncs at 6 s (an LPA within the slot) and 600 s; ACKs (LUTT-IE, frame type
 * 5) at 467 s, 4 967 s and 15 767 s; the rate first measured at the 10 h
 * sync (36 600 s); then 10 h without a frame. Every window after the 10 h
 * sync must hold the parent's TX at the slot start of its view. Before the
 * fix the last ACK became the view, and our slots kept the 0.24 s they had
 * drifted from the parent's view by 15 767 s: every window missed. */
static void test_ack_not_view(void)
{
    const uint32_t iv_ms = 300000, bc_ms = 600000, guard = 1000;
    const double k = 1.0 + 15600e-9;          // our µs per parent µs
    fhss_ws_lfn_listen_t cfg;
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = iv_ms;
    cfg.rx_guard_us = guard;
    cfg.bc_sync_only = true;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t L0 = now_us + 1000000;            // the parent's broadcast slot 0
    #define PS(k_) (L0 + (uint64_t)((double)(k_) * bc_ms * 1000 * k))
    #define AT(s_) (L0 + (uint64_t)((double)(s_) * 1e6 * k))
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 60, .channel_function = WS_DH1CF };
    // an LPA within broadcast slot 0, 6 s in
    now_us = AT(6);
    sy.slot = 0;
    sy.offset_ms = 6000;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t lutt_t = 0;
    uint32_t lutt_off = 0;
    uint16_t lutt_slot = 0;
    // the data frame at 167 s: the parent's view
    while (step(AT(167))) {
    }
    now_us = AT(167);
    api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, ts());
    api->data_tx_done(api, false, true, 1);     // acknowledged
    lutt_t = now_us;
    lutt_slot = ie[4] | ie[5] << 8;
    lutt_off = ie[6] | ie[7] << 8 | ie[8] << 16;
    // ACKs and the syncs at slots 1 (600 s, a broadcast LPC) and 61 (10 h)
    const double acks[3] = { 467, 4967, 15767 };
    int a = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t slot = i ? 61 : 1;
        for (; a < 3 && AT(acks[a]) < PS(slot); a++) {
            while (step(AT(acks[a]))) {
            }
            now_us = AT(acks[a]);
            uint8_t ack[16];
            write_lutt_ie(ack);
            ack[3] = 5;     // WS_FT_ACK
            api->write_synch_info(api, ack, len, FHSS_DATA_FRAME, ts());
        }
        while (step(PS(slot) - 1)) {
        }
        now_us = PS(slot) + 500;
        sy.slot = slot;
        sy.offset_ms = 0;
        sy.rx_timestamp_us = ts();
        CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    }
    CHECK(ns_fhss_ws_lfn_stats_get(api, &st) == 0);
    CHECK(st.rate_ppb > 15400 && st.rate_ppb < 15800);
    // the windows of the next 10 h (no frame, no sync)
    nevs = 0;
    while (step(PS(120) - 2000000)) {
    }
    int seen = 0, bad = 0;
    for (int i = 0; i + 1 < nevs; i++) {
        if (evs[i].e != FHSS_RX_WINDOW_OPEN || evs[i + 1].e != FHSS_RX_WINDOW_CLOSE) {
            continue;
        }
        double open = (double)evs[i].t, close = (double)evs[i + 1].t;
        double s0 = (double)lutt_t - (double)lutt_off * 1000 * k;
        long n = lround((open + guard - s0) / ((double)iv_ms * 1000 * k));
        double view = s0 + (double)n * iv_ms * 1000 * k;
        bool in = view >= open && view + 1000 <= close;
        if (!in && bad++ < 3) {
            fprintf(stderr, "ack_not_view: window %ld: the parent's TX %.0f us from the window's opening, window %.0f us\n",
                    n, view - open, close - open);
        }
        CHECK(in);
        CHECK(evs[i].channel == dh1cf_channel((uint16_t)(lutt_slot + n), lfn_mac));
        seen++;
    }
    CHECK(seen >= 115);
    #undef PS
    #undef AT
    CHECK(ns_fhss_delete(api) == 0);
}

/* Only a frame that reached the parent sets its view. The timeline of
 * test_ack_not_view (Eco, the parent's clock 15.6 ppm off ours, the rate
 * first measured at the 10 h sync): a data frame at 167 s, acknowledged
 * (the view); later data frames (LUTT-IE) whose fate the MAC reports:
 *   mode 0: at 4 967 s, CSMA failed (never on the air); at 15 767 s, one
 *           attempt on the air, unacknowledged: neither is the view;
 *   mode 1: at 15 767 s, two attempts on the air (the second 20 ms later),
 *           unacknowledged: the view (the parent more likely has one);
 *   mode 2: at 15 767 s, an LPCS (no ACK requested), sent: the view;
 *   mode 3: at 15 767 s, a frame to another node than the parent (an old
 *           parent's release), acknowledged: not the view (before the fix
 *           it was: every window 0.24 s off);
 *   mode 4: at 15 767 s, a frame whose CSMA failed for good, then the TX
 *           done of an asynchronous buffer that expired in the queue
 *           (another handle, no frame built): not the view (before the
 *           fix it was);
 *   mode 6: as mode 0, the parent's FFN broadcast schedule unknown when
 *           it is set (ns_fhss_ws_set_parent() with interval 0): the frame
 *           at 167 s to it is the view (ab64f8b: before, the parent's
 *           address was stored only with a valid broadcast schedule);
 *   mode 7: at 15 767 s, a frame that fails with no report naming its
 *           handle, then an expired async buffer's TX done: not the view
 *           (before the fix the handle was learnt from reports only);
 *   mode 8: at 15 767 s, a frame with handle 0xff, reports with handles 0
 *           and 0xff that are not its, then on the air and acknowledged:
 *           the view;
 *   mode 5: at 15 767 s, a CSMA failure, the frame built again, a clear
 *           channel report (PHY_LINK_CCA_OK: no ACK, not completed), on the
 *           air and acknowledged: the view (the second build).
 * Every window in the 10 h after the rate's sync must hold the parent's TX
 * at the slot start of the expected view. Before the fix the view was set
 * when a frame was built: in mode 0 our slots kept the 0.24 s they had
 * drifted from the parent's view by 15 767 s, and every window missed. */
static void test_view_on_tx_done(int mode)
{
    const uint32_t iv_ms = 300000, bc_ms = 600000, guard = 1000;
    const double k = 1.0 + 15600e-9;          // our µs per parent µs
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = iv_ms;
    cfg.rx_guard_us = guard;
    cfg.bc_sync_only = true;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    uint64_t L0 = now_us + 1000000;            // the parent's broadcast slot 0
    #define PS(k_) (L0 + (uint64_t)((double)(k_) * bc_ms * 1000 * k))
    #define AT(s_) (L0 + (uint64_t)((double)(s_) * 1e6 * k))
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 60, .channel_function = WS_DH1CF };
    now_us = AT(6);
    sy.slot = 0;
    sy.offset_ms = 6000;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    uint8_t ie[16];
    int len = write_lutt_ie(ie);
    uint64_t view_t = 0;
    uint32_t view_off = 0;
    uint16_t view_slot = 0;
    // a frame built at time s_ (its LUTT-IE written), handle h_
    #define BUILD(s_, h_) do { \
            while (step(AT(s_))) { \
            } \
            now_us = AT(s_); \
            write_lutt_ie(ie); \
            if (api->tx_frame) { \
                api->tx_frame(api, h_); \
            } \
            api->write_synch_info(api, ie, len, FHSS_DATA_FRAME, ts()); \
        } while (0)
    #define VIEW() do { \
            view_t = now_us; \
            view_slot = ie[4] | ie[5] << 8; \
            view_off = ie[6] | ie[7] << 8 | ie[8] << 16; \
        } while (0)
    // our parent (the frames to it: tx_handle with its address); mode 6:
    // its FFN broadcast schedule not known (no BT-IE yet): the parent is
    // still ours
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    broadcast_timing_info_t pbt = { .broadcast_channel_function = WS_DH1CF, .broadcast_dwell_interval = 255, .broadcast_interval = mode == 6 ? 0 : 1020 };
    CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, false) == 0);
    static const uint8_t other_mac[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x07 };
    BUILD(167, 7);
    api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts());
    api->data_tx_done(api, true, false, 7);    // on the air
    api->data_tx_done(api, false, true, 7);    // acknowledged
    VIEW();
    for (int i = 0; i < 2; i++) {
        uint16_t slot = i ? 61 : 1;
        if (i == 1) {
            if (mode == 0 || mode == 6) {
                BUILD(4967, 8);
                api->data_tx_done(api, false, false, 8);    // CSMA failed
                BUILD(15767, 9);
                api->data_tx_done(api, true, false, 9);     // on the air
                api->data_tx_done(api, false, false, 9);    // no ACK, given up
            } else if (mode == 1) {
                BUILD(15767, 9);
                api->data_tx_done(api, true, false, 9);
                BUILD(15767.02, 9);                           // the retry
                api->data_tx_done(api, true, false, 9);
                VIEW();
                api->data_tx_done(api, false, false, 9);
            } else if (mode == 2) {
                BUILD(15767, 10);
                api->data_tx_done(api, false, true, 10);    // sent, no ACK requested
                VIEW();
            } else if (mode == 4) {
                BUILD(15767, 12);
                api->data_tx_done(api, false, false, 12);   // CSMA failed, given up
                api->data_tx_done(api, false, true, 13);    // an expired async buffer
            } else if (mode == 7) {
                BUILD(15767, 14);
                // it fails with no report naming it (a TX too late, a TX
                // not allowed, a timeout), then an expired async buffer
                api->data_tx_done(api, false, true, 15);
            } else if (mode == 8) {
                BUILD(15767, 0xff);                         // handles wrap through 0xff and 0
                api->data_tx_done(api, false, false, 0);    // a frame received
                api->data_tx_done(api, false, false, 0xff); // our ACK's TX done
                api->data_tx_done(api, true, false, 0xff);  // on the air
                api->data_tx_done(api, false, true, 0xff);  // acknowledged
                VIEW();
            } else if (mode == 5) {
                BUILD(15767, 12);
                api->data_tx_done(api, false, false, 12);   // CSMA failed
                BUILD(15767.01, 12);                           // built again
                api->data_tx_done(api, false, false, 12);   // channel clear (PHY_LINK_CCA_OK)
                api->data_tx_done(api, false, false, 0);    // a frame received meanwhile
                api->data_tx_done(api, true, false, 12);    // on the air
                api->data_tx_done(api, false, true, 12);    // acknowledged
                VIEW();
            } else {
                BUILD(15767, 11);
                api->tx_handle(api, false, (uint8_t *)other_mac, FHSS_DATA_FRAME, 100, 0, 0, ts());
                api->data_tx_done(api, true, false, 11);    // on the air
                api->data_tx_done(api, false, true, 11);    // acknowledged by that node
            }
        }
        while (step(PS(slot) - 1)) {
        }
        now_us = PS(slot) + 500;
        sy.slot = slot;
        sy.offset_ms = 0;
        sy.rx_timestamp_us = ts();
        CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    }
    nevs = 0;
    while (step(PS(120) - 2000000)) {
    }
    int seen = 0, bad = 0;
    for (int i = 0; i + 1 < nevs; i++) {
        if (evs[i].e != FHSS_RX_WINDOW_OPEN || evs[i + 1].e != FHSS_RX_WINDOW_CLOSE) {
            continue;
        }
        double open = (double)evs[i].t, close = (double)evs[i + 1].t;
        double s0 = (double)view_t - (double)view_off * 1000 * k;
        long n = lround((open + guard - s0) / ((double)iv_ms * 1000 * k));
        double view = s0 + (double)n * iv_ms * 1000 * k;
        bool in = view >= open && view + 1000 <= close;
        if (!in && bad++ < 3) {
            fprintf(stderr, "view_on_tx_done mode %d: window %ld: the parent's TX %.0f us from the window's opening, window %.0f us\n",
                    mode, n, view - open, close - open);
        }
        CHECK(in);
        CHECK(evs[i].channel == dh1cf_channel((uint16_t)(view_slot + n), lfn_mac));
        seen++;
    }
    CHECK(seen >= 115);
    #undef BUILD
    #undef VIEW
    #undef PS
    #undef AT
    CHECK(ns_fhss_delete(api) == 0);
}

/* A sync slot's broadcast window against our own unicast window (emulator
 * Eco seed 2, stack 60ce54d): Eco settings (300 s listen interval, 600 s LFN
 * broadcast, a sync frame every 60th slot, listening only in those), the
 * parent's clock 15.6 ppm off ours, the rate not measured yet (one sync
 * from an LPA). 10 h later both windows have widened by 50 ppm (1.8 s each
 * side) and overlap: our unicast slot starts 0.5 s before the sync slot as
 * we predict it, so its window opens first. The parent's LTS, 0.56 s after
 * the predicted start, must find a broadcast window open on the broadcast
 * channel; the unicast windows go on after it, whether or not the LTS
 * comes.
 * Before the fix the broadcast window was skipped while the unicast one
 * held the radio, and the sync came 20 h after the previous one. */
/* mode 0: the LTS comes; 1: lost; 2: lost, the broadcast widening bound
 * at 0.5 s: the rest of the unicast window opens after the broadcast one;
 * 3: the LTS comes, our unicast slot 3 s after the predicted sync slot (the
 * broadcast window opens first; our window's time comes during it, after
 * the LTS); 4: as 3, then the parent lost while the broadcast window is
 * open, after our slot's time in it; 5: as 1, then the parent lost while
 * the broadcast window is open: the cut is forgotten (white box: before the
 * fix the next broadcast window's close, in the same own slot, could reopen
 * a unicast window from it); 6: the sync 0.56 s early (our clock slow),
 * our slot 1.5 s after the predicted sync slot, then the parent lost in
 * the broadcast window during a frame (close deferred once); 7: as 6, then
 * our schedule set again (an LTO-IE) while the close is retried.
 * When the LTS comes, our next unicast slots must move by its phase error:
 * before the fix the unicast timer, still running when the sync came,
 * re-anchored our slots at the old slot in the broadcast window (the skip
 * branch) and the correction was lost (562 ms off). */
static void test_sync_over_uc(int mode)
{
    bool lts_lost = mode == 1 || mode == 2 || mode == 5;
    const uint32_t iv_ms = 300000, bc_ms = 600000, guard = 1000;
    // our µs per parent µs (mode 6: our clock slow, the sync early)
    const double k = mode >= 6 ? 1.0 - 15600e-9 : 1.0 + 15600e-9;
    fhss_ws_lfn_listen_t cfg;
    fhss_ws_lfn_stats_t st;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = iv_ms;
    cfg.rx_guard_us = guard;
    cfg.bc_sync_only = true;
    uint64_t L0 = now_us + 3000000;            // the parent's broadcast slot 0
    // our slot 120 starts 0.5 s before the sync slot 60 as we predict it
    // (mode 3: 3 s after it)
    cfg.first_slot_delay_us = mode == 3 || mode == 4 ? 6000000 : mode >= 6 ? 4500000 : 2500000;
    if (mode == 2) {
        cfg.bc_widening_max_us = 500000;
    }
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    #define PS(k_) (L0 + (uint64_t)((double)(k_) * bc_ms * 1000 * k))
    fhss_ws_lfn_bc_sync_t sy = { .interval_ms = bc_ms, .bsi = 0x77, .sync_period = 60, .channel_function = WS_DH1CF };
    // an LPA 4 s into the parent's broadcast slot 0
    while (step(L0 + 4000000)) {
    }
    now_us = L0 + 4000000;
    sy.slot = 0;
    sy.offset_ms = 4000;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    nevs = 0;
    uint64_t t_lts = PS(60) + 500;
    while (step(t_lts)) {
    }
    now_us = t_lts;
    // the radio at the LTS: the last window opened is the broadcast one
    int last_open = -1;
    bool closed = false;
    for (int i = 0; i < nevs; i++) {
        if (evs[i].t < PS(60) - 10000000) {
            continue;
        }
        if (evs[i].e == FHSS_RX_WINDOW_OPEN) {
            last_open = i;
            closed = false;
        } else if (evs[i].e == FHSS_RX_WINDOW_CLOSE && !evs[i].ret) {
            closed = true;
        }
    }
    bool bc_open = last_open >= 0 && !closed && evs[last_open].channel == bc_channel(60, 0x77);
    if (mode == 2) {
        // the LTS falls after this narrower broadcast window: the rest of
        // the unicast window of slot 120 is open on its channel
        bc_open = last_open >= 2 && !closed && evs[last_open].channel == dh1cf_channel(120, lfn_mac) &&
                  evs[last_open - 1].e == FHSS_RX_WINDOW_CLOSE && evs[last_open - 2].channel == bc_channel(60, 0x77);
    }
    if (!bc_open) {
        fprintf(stderr, "sync_over_uc: at the LTS (%.3f s): ", (double)(t_lts - L0) / 1e6);
        for (int i = 0; i < nevs; i++) {
            if (evs[i].t >= PS(60) - 10000000) {
                fprintf(stderr, "%s %.3f ch %d; ", evs[i].e == FHSS_RX_WINDOW_OPEN ? "open" : "close",
                        ((double)evs[i].t - (double)L0) / 1e6, evs[i].channel);
            }
        }
        fprintf(stderr, "\n");
    }
    CHECK(bc_open);
    if (mode == 5) {
        struct fhss_ws_lfn *lfn = &fhss_get_object_with_api(api)->ws->lfn;
        CHECK(lfn->uc_cut_for_sync);
        CHECK(ns_fhss_ws_lfn_bc_stop(api) == 0);
        CHECK(!lfn->uc_cut_for_sync);
        CHECK(ns_fhss_delete(api) == 0);
        return;
    }
    if (lts_lost) {
        /* the LTS lost: the broadcast window outlasts what was left of our
           unicast window (both widen alike), and our next unicast window
           (slot 121) opens on its channel; with a broadcast widening bound
           below the unicast one, the rest of the unicast window would open
           after the broadcast window */
        int n1 = nevs;
        while (step(PS(60) + 303000000)) {
        }
        bool resumed = false;
        for (int i = n1; i < nevs; i++) {
            resumed |= evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].channel == dh1cf_channel(121, lfn_mac) &&
                       evs[i].t > PS(60) + 290000000;
        }
        if (!resumed) {
            for (int i = n1; i < nevs; i++) {
                fprintf(stderr, "  lts_lost: %s %.3f ch %d ret %d\n", evs[i].e == FHSS_RX_WINDOW_OPEN ? "open" : "close", ((double)evs[i].t - (double)L0) / 1e6, evs[i].channel, evs[i].ret);
            }
        }
        CHECK(resumed);
        CHECK(ns_fhss_delete(api) == 0);
        return;
    }
    // the LTS: a sync, 0.56 s from our prediction (the rate needs a second
    // slot-start frame)
    sy.slot = 60;
    sy.offset_ms = 0;
    sy.rx_timestamp_us = ts();
    CHECK(ns_fhss_ws_lfn_bc_sync(api, &sy) == 0);
    CHECK(ns_fhss_ws_lfn_stats_get(api, &st) == 0);
    int32_t err = mode >= 6 ? -st.last_sync_error_us : st.last_sync_error_us;
    CHECK(st.bc_syncs == 2 && err > 550000 && err < 575000);
    int n0 = nevs;
    if (mode >= 6) {
        /* the parent lost while the broadcast window is open, during a frame
           (the MAC defers the close once): the close retry must not bring
           back our slots of before the sync */
        while (step(PS(60) + 2160000)) {
        }
        now_us = PS(60) + 2160000;
        close_defer = 1;
        CHECK(ns_fhss_ws_lfn_bc_stop(api) == 0);
    }
    if (mode == 7) {
        /* then our schedule set again at once (an LPA's LTO-IE: offset 20 s
           from now): the close is still retried, and our next window, of
           the new schedule, opens */
        uint64_t t_stop = now_us;
        CHECK(ns_fhss_ws_lfn_timing_set(api, 20000, 0, ts()) == 0);
        while (step(t_stop + 700000000)) {
        }
        uint64_t closed_at = 0;
        int opens = 0;
        for (int i = n0; i < nevs; i++) {
            if (!closed_at && evs[i].t >= t_stop && evs[i].e == FHSS_RX_WINDOW_CLOSE && !evs[i].ret) {
                closed_at = evs[i].t;
            }
            opens += evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].t > t_stop;
        }
        if (!closed_at || closed_at - t_stop > 3 * cfg.close_retry_us || opens < 2) {
            fprintf(stderr, "sync_over_uc mode 7: radio closed %.3f s after the stop, %d windows opened\n",
                    closed_at ? (double)(closed_at - t_stop) / 1e6 : -1.0, opens);
        }
        CHECK(closed_at && closed_at - t_stop <= 3 * cfg.close_retry_us);
        // the windows of the new schedule: 20 s after the LTO, then every 300 s
        CHECK(opens >= 2);
        bool first = false;
        for (int i = n0; i < nevs; i++) {
            first |= evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].t > t_stop + 19000000 && evs[i].t < t_stop + 20000000;
        }
        CHECK(first);
        CHECK(ns_fhss_delete(api) == 0);
        return;
    }
    if (mode == 4) {
        /* the parent lost while the broadcast window is open, after our
           slot's time in it (skipped): our slots go on without that
           window's close */
        while (step(PS(60) + 1000000)) {
        }
        now_us = PS(60) + 1000000;
        CHECK(ns_fhss_ws_lfn_bc_stop(api) == 0);
        while (step(PS(61))) {
        }
        int opens = 0;
        for (int i = n0; i < nevs; i++) {
            opens += evs[i].e == FHSS_RX_WINDOW_OPEN && evs[i].t > PS(60) + 1000000;
        }
        CHECK(opens >= 2);
        CHECK(ns_fhss_delete(api) == 0);
        return;
    }
    // the broadcast window closes; our unicast windows go on, moved by the
    // sync's error: slot n at its old start + the error
    while (step(mode == 6 ? PS(62) : PS(61))) {
    }
    int uc_after = 0;
    double slot0 = (double)L0 - 3000000 + cfg.first_slot_delay_us;
    for (int i = n0; i + 1 < nevs; i++) {
        if (evs[i].e != FHSS_RX_WINDOW_OPEN || evs[i + 1].e != FHSS_RX_WINDOW_CLOSE ||
                evs[i].channel == bc_channel(61, 0x77)) {
            continue;
        }
        uc_after++;
        // the slot start, from the window: open = start - guard - w,
        // close = start - guard + rx_window + w
        double start = ((double)evs[i].t + (double)evs[i + 1].t - cfg.rx_window_us) / 2 + guard;
        long n = lround((start - slot0) / (iv_ms * 1000.0));
        double expect = slot0 + (double)n * iv_ms * 1000 + st.last_sync_error_us;
        if (fabs(start - expect) > 1000) {
            fprintf(stderr, "sync_over_uc mode %d: slot %ld starts %.0f us from the sync's correction\n", mode, n, start - expect);
        }
        CHECK(fabs(start - expect) <= 1000);
        CHECK(evs[i].channel == dh1cf_channel((uint16_t)n, lfn_mac));
    }
    CHECK(uc_after >= 2);
    #undef PS
    CHECK(ns_fhss_delete(api) == 0);
}

/* The rate of the parent's FFN unicast schedule, measured from its UTT-IEs
 * apart from the rate against its LFN broadcast schedule (a router's FFN
 * hopping follows the border router's time base). The parent's schedule
 * runs 28 ppm slow against our clock; no LFN broadcast sync (rate_ppb 0).
 * UTT-IEs at 10 s, 30 min and 2 h; at 6 h the LFN's re-anchored reference
 * (time, UFSI) of the parent's schedule must still match the parent's: a few
 * UFSI units (256 per dwell, about 1 ms each). Before the fix the
 * re-anchoring ran at rate_ppb: 4 h at 28 ppm, about 400 ms (400 units). */
static void test_ffn_rate(void)
{
    const double k = 1.0 - 28000e-9;      // our µs per parent µs
    const uint32_t dwell_ms = 255;
    fhss_ws_lfn_listen_t cfg;
    fhss_api_t *api;

    reset_fakes();
    api = create_fhss(lfn_mac, 255, 0, 0);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    ns_fhss_ws_lfn_listen_defaults(&cfg);
    cfg.listen_interval_ms = 4 * 3600 * 1000;
    CHECK(ns_fhss_ws_lfn_listen_set(api, &cfg) == 0);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    memset(&parent_neighbor, 0, sizeof(parent_neighbor));
    parent_neighbor_known = true;
    unicast_timing_info_t *u = &parent_neighbor.uc_timing_info;
    u->unicast_channel_function = WS_DH1CF;
    u->unicast_dwell_interval = dwell_ms;
    u->unicast_number_of_channels = 35;
    parent_neighbor.uc_channel_list.channel_count = 35;
    for (int i = 0; i < 35; i++) {
        parent_neighbor.uc_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    broadcast_timing_info_t pbt = { .broadcast_channel_function = WS_DH1CF, .broadcast_dwell_interval = 255,
                                    .broadcast_interval = 1020, .bt_rx_timestamp = ts() };
    CHECK(ns_fhss_ws_set_parent(api, parent_mac, &pbt, true) == 0);
    // The parent's UFSI at our time t (µs): its ms since an origin, 256 per dwell
    const uint64_t T0 = now_us;
    #define UFSI(t_) ((uint32_t)fmod(floor(((double)(t_) - (double)T0) / k / 1000.0 * 256.0 / dwell_ms) + 123456.0, 16777216.0))
    const uint64_t at[3] = { 10000000ull, 1800000000ull, 7200000000ull };
    for (int i = 0; i < 3; i++) {
        while (step(T0 + at[i])) {
        }
        now_us = T0 + at[i];
        u->utt_rx_timestamp = ts();
        u->ufsi = UFSI(now_us);
        // the next timer event sees it (the LFN hops every 2^29 µs at most)
    }
    while (step(T0 + 6ull * 3600000000ull)) {
    }
    // The reference as the LFN left it, against the parent's UFSI then
    uint64_t ref_t = now_us - (uint32_t)(ts() - u->utt_rx_timestamp);
    int32_t err = (int32_t)((u->ufsi - UFSI(ref_t)) << 8) >> 8;
    if (err < -6 || err > 6) {
        fprintf(stderr, "parent FFN reference %d UFSI units off at 6 h\n", err);
    }
    CHECK(err >= -6 && err <= 6);
    CHECK((uint32_t)(ts() - u->utt_rx_timestamp) < 0x40000000u);     // kept recent
    #undef UFSI
    parent_neighbor_known = false;
    CHECK(ns_fhss_delete(api) == 0);
}

#ifndef OLD_STACK
/* The time to an LFN frame's next TX opportunity (lfn_tx_delay), used by
 * the MAC to hold an asynchronous sweep: the LFN's next slot start + the TX
 * offset, at least tx_min_prep_us away; 0 during a frame-pending burst;
 * false for an FFN destination or without the parent role */
static void test_tx_delay(void)
{
    const uint32_t interval_ms = 2000;
    fhss_ws_lfn_parent_t pcfg;
    fhss_api_t *api;
    uint32_t d = 1;

    reset_fakes();
    api = create_fhss(parent_mac, 255, 1020, 255);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
    api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
    CHECK(api->lfn_tx_delay != NULL);
    memset(&lfn_neighbor, 0, sizeof(lfn_neighbor));
    lfn_neighbor_known = true;
    lfn_neighbor.uc_timing_info.unicast_channel_function = WS_DH1CF;
    lfn_neighbor.lfn_uc_timing_info.listen_interval_ms = interval_ms;
    lfn_neighbor.lfn_uc_timing_info.slot_number = 7;
    lfn_neighbor.lfn_uc_timing_info.interval_offset_ms = 500;     // slot 7 started 500 ms ago
    lfn_neighbor.lfn_uc_timing_info.lutt_rx_timestamp = ts();
    CHECK(!api->lfn_tx_delay(api, lfn_mac, &d));                    // no parent role yet
    ns_fhss_ws_lfn_parent_defaults(&pcfg);
    CHECK(ns_fhss_ws_lfn_parent_set(api, &pcfg) == 0);
    CHECK(api->lfn_tx_delay(api, lfn_mac, &d));
    CHECK(d == 1500000u + pcfg.tx_offset_us);                       // slot 8
    now_us += 1500000u - pcfg.tx_min_prep_us / 2;                   // too close to slot 8: slot 9
    CHECK(api->lfn_tx_delay(api, lfn_mac, &d));
    CHECK(d == interval_ms * 1000u + pcfg.tx_min_prep_us / 2 + pcfg.tx_offset_us);
    CHECK(!api->lfn_tx_delay(api, parent_mac, &d));                 // not an LFN
    CHECK(!api->lfn_tx_delay(api, NULL, &d));                       // no LFN broadcast schedule
    // A burst (the LFN acknowledged a frame with the pending bit): now
    api->lfn_pending(api, lfn_mac);
    CHECK(api->lfn_tx_delay(api, lfn_mac, &d) && d == 0);
    lfn_neighbor_known = false;
    CHECK(ns_fhss_delete(api) == 0);
}
#endif

int main(void)
{
    test_sched();
    test_lfn();
    test_lfn_long();
    test_long_interval();
    test_parent();
    test_bc_parent();
    test_bc_lfn(false);
    test_bc_lfn(true);
    test_discovery();
    test_own_ref();
    test_own_ref_jitter(0, 12345);
    {
        static const uint32_t lost[] = { 35, 65, 105, 155, 205, 305, 505, 1005 };
        for (unsigned i = 0; i < sizeof(lost) / sizeof(lost[0]); i++) {
            for (uint32_t seed = 1; seed <= 4; seed++) {
                test_own_ref_jitter(lost[i], seed * 12345);
            }
        }
    }
    test_ack_not_view();
    test_view_on_tx_done(0);
    test_view_on_tx_done(1);
    test_view_on_tx_done(2);
    test_view_on_tx_done(3);
    test_view_on_tx_done(4);
    test_view_on_tx_done(5);
    test_view_on_tx_done(6);
    test_view_on_tx_done(7);
    test_view_on_tx_done(8);
    test_sync_over_uc(0);
    test_sync_over_uc(1);
    test_sync_over_uc(2);
    test_sync_over_uc(3);
    test_sync_over_uc(4);
    test_sync_over_uc(5);
    test_sync_over_uc(6);
    test_sync_over_uc(7);
    test_sync_in_discovery();
    test_ffn_rate();
#ifndef OLD_STACK
    test_tx_delay();
#endif
    printf("fhss_lfn: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
