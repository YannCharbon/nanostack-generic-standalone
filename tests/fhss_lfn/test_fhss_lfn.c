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

#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_config.h"
#include "nanostack/mac/fhss_ws_extension.h"
#include "nanostack/mac/net_fhss.h"
#include "fhss/fhss_ws_lfn.h"
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

/* Neighbour table of the parent: one entry, the LFN */
static fhss_ws_neighbor_timing_info_t lfn_neighbor;
static bool lfn_neighbor_known;
static fhss_ws_neighbor_timing_info_t *get_neighbor(const fhss_api_t *a, uint8_t eui64[8])
{
    (void)a;
    if (lfn_neighbor_known && !memcmp(eui64, lfn_mac, 8)) {
        return &lfn_neighbor;
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
    uint32_t off;

    CHECK(!fhss_lfn_sched_from_lutt(&s, 0, 0, 0, 0));
    CHECK(!fhss_lfn_sched_from_lutt(&s, 1000, 0, 1000, 0));          // offset outside the interval
    CHECK(!fhss_lfn_sched_from_lutt(&s, 2147484, 0, 0, 0));          // above 2^31 µs
    CHECK(fhss_lfn_sched_from_lutt(&s, 2147483, 0, 0, 0));

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
        CHECK(ns_fhss_ws_lfn_timing_set(api, 0, 2147484, 0) == -2);
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

    // TTL: a frame queued more than tx_ttl_intervals listen intervals ago
    uint32_t old = ts() - (uint32_t)pcfg.tx_ttl_intervals * interval_ms * 1000u - 1;
    CHECK(api->lfn_tx_check(api, lfn_mac, old, &tx_time) == FHSS_LFN_TX_EXPIRED);
    ns_fhss_ws_lfn_stats_get(api, &st);
    CHECK(st.tx_released == 12 && st.tx_expired == 1);

    // An FFN destination still takes the FFN path: its UTT schedule, unknown
    // here, so tx_handle says "unknown neighbour" (-2), as before
    CHECK(api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts()) == -2 ||
          api->tx_handle(api, false, (uint8_t *)parent_mac, FHSS_DATA_FRAME, 100, 0, 0, ts()) == -1);

    // Disabled again: NOT_LFN
    CHECK(ns_fhss_ws_lfn_parent_set(api, NULL) == 0);
    CHECK(api->lfn_tx_check(api, lfn_mac, ts(), &tx_time) == FHSS_LFN_TX_NOT_LFN);
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

int main(void)
{
    test_sched();
    test_lfn();
    test_lfn_long();
    test_parent();
    printf("fhss_lfn: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
