/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the idle-poll skipping of the FHSS TX queue (docs/PORTING.md,
 * "Idle-poll skipping of the FHSS TX queue"; NS_FHSS_POLL_SKIP_IDLE). The
 * stack's real FHSS (rcp/fhss/) runs on a fake port (a virtual µs clock that
 * starts near the 32-bit wrap, FHSS timers and the event loop's callback
 * timer with absolute deadlines) and a fake MAC (a unicast TX queue). Built
 * twice from this file: with the periodic poll, and with skipping and µs
 * callback timers (NS_PORT_TIMER_USEC, as the POSIX, simulation and FG25
 * ports).
 *
 * A schedule of events over 20 virtual minutes moves the FHSS through its
 * states as a router's bootstrap does: fixed channel before the schedules
 * start (the idle state, where the 10 ms poll runs), unicast hopping,
 * synchronized with a broadcast schedule and a parent, expedited forwarding,
 * unsynchronized, and back. It queues frames at arbitrary instants (the fake
 * MAC tells the FHSS through fhss_api->tx_queued, as mcps_sap_pd_req_queue_write()
 * does), and the fake MAC's tx_poll sends or keeps the first frame by a
 * random draw (a frame that cannot go yet, as a blacklisted destination).
 *
 * Each program prints one line per poll (time, queue) and per state change.
 * run.sh requires both outputs to be identical: the skipped polls are those
 * that would have found the queue empty, and every other poll happens at the
 * same µs. The skipping program also checks that it woke far less often in
 * the idle state.
 *
 *   tests/fhss_poll/run.sh [build dir]
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

static uint64_t now_us;
#define CLOCK32_OFFSET_US (UINT32_MAX - 30000000u)   // wraps 30 s into the run
static uint32_t ts(void) { return (uint32_t)(now_us + CLOCK32_OFFSET_US); }

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s (t=%" PRIu64 " us)\n", __FILE__, __LINE__, #cond, now_us); \
            exit(1); \
        } \
    } while (0)

/* ----------------------------------------------------------- fake port */

void platform_enter_critical(void) { }
void platform_exit_critical(void) { }
void mbed_tracef(uint8_t dlevel, const char *grp, const char *fmt, ...) { (void)dlevel; (void)grp; (void)fmt; }
char *mbed_trace_array(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return ""; }
char *trace_array(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return ""; }

static uint64_t rng_mod = 777;      // drawn by the FHSS and the fake MAC
static uint64_t rng_sched = 12345;  // drawn by the schedule only
static uint32_t rnd(uint64_t *rng, uint32_t n)
{
    *rng = *rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(*rng >> 33) % n;
}
uint16_t rand_get_random_in_range(uint16_t min, uint16_t max) { return min + rnd(&rng_mod, max - min + 1); }

// The event loop's callback timer: only the FHSS's TX poll timer uses one
static void (*poll_timer_fn)(int, uint16_t);
static bool poll_timer_armed;
static uint64_t poll_timer_deadline;
static unsigned long poll_timer_fires, idle_fires;
int eventOS_callback_timer_register(void (*fn)(int, uint16_t)) { poll_timer_fn = fn; return 1; }
int eventOS_callback_timer_start(int id, uint16_t slots)
{
    (void)id;
    poll_timer_armed = slots != 0;
    poll_timer_deadline = now_us + (uint64_t)slots * 50;
    return 0;
}
int eventOS_callback_timer_start_usec(int id, int usec)
{
    (void)id;
    CHECK(usec >= 0);
    poll_timer_armed = usec != 0;
    poll_timer_deadline = now_us + (uint64_t)usec;
    return 0;
}
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
    CHECK(i < FHSS_TIMERS);
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

/* ------------------------------------------------------------ fake MAC */

static const uint8_t own_mac[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x07 };
static const uint8_t parent_mac[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x00 };
static fhss_api_t *api;
static uint16_t queue;
static unsigned long polls;

static uint16_t m_queue_size(const fhss_api_t *a, bool bc) { (void)a; return bc ? 0 : queue; }
static int m_mac_address(const fhss_api_t *a, uint8_t *mac) { (void)a; memcpy(mac, own_mac, 8); return 0; }
static uint32_t m_datarate(const fhss_api_t *a) { (void)a; return 150000; }
static uint32_t m_timestamp(const fhss_api_t *a) { (void)a; return ts(); }
static int m_change_channel(const fhss_api_t *a, uint8_t ch) { (void)a; (void)ch; return 0; }
static int m_bc_notify(const fhss_api_t *a, uint32_t t) { (void)a; (void)t; return 0; }
static int m_coord(const fhss_api_t *a, uint8_t *mac) { (void)a; (void)mac; return 0; }

// The MAC reads its queue: the first frame goes, or waits (a frame that
// cannot be sent yet), by a random draw
static int m_tx_poll(const fhss_api_t *a)
{
    (void)a;
    polls++;
    printf("t%" PRIu64 " poll q=%u", now_us, queue);
    if (queue && rnd(&rng_mod, 4)) {
        queue--;
        printf(" sent");
    }
    putchar('\n');
    return 0;
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
};

static fhss_ws_neighbor_timing_info_t *get_neighbor(const fhss_api_t *a, uint8_t eui64[8])
{
    (void)a;
    (void)eui64;
    return NULL;
}

/* ---------------------------------------------------------- the states */

// EU-like plan, 35 channels. Fixed: before the schedules start
static void fhss_config(fhss_ws_configuration_t *c, bool fixed, bool bc)
{
    memset(c, 0, sizeof(*c));
    c->ws_uc_channel_function = fixed ? WS_FIXED_CHANNEL : WS_DH1CF;
    c->ws_bc_channel_function = fixed ? WS_FIXED_CHANNEL : WS_DH1CF;
    c->unicast_fixed_channel = 7;
    c->broadcast_fixed_channel = 7;
    c->bsi = 0x1234;
    c->fhss_uc_dwell_interval = fixed ? 0 : 255;
    c->fhss_broadcast_interval = bc ? 1020 : 0;
    c->fhss_bc_dwell_interval = bc ? 255 : 0;
    c->channel_mask_size = 35;
    for (int i = 0; i < 35; i++) {
        c->channel_mask[i / 32] |= 1u << (i % 32);
        c->unicast_channel_mask[i / 32] |= 1u << (i % 32);
    }
}

enum { S_IDLE, S_HOPPING, S_SYNCHRONIZED, S_UNSYNCHRONIZED, S_COUNT };
static int state;

static void state_enter(int s)
{
    fhss_ws_configuration_t c;

    state = s;
    switch (s) {
        case S_IDLE:
            fhss_config(&c, true, false);
            CHECK(ns_fhss_ws_configuration_set(api, &c) == 0);
            printf("t%" PRIu64 " idle\n", now_us);
            break;
        case S_HOPPING:
            fhss_config(&c, false, false);
            CHECK(ns_fhss_ws_configuration_set(api, &c) == 0);
            printf("t%" PRIu64 " hopping\n", now_us);
            break;
        case S_SYNCHRONIZED: {
            broadcast_timing_info_t bt = {
                .broadcast_channel_function = WS_DH1CF,
                .broadcast_dwell_interval = 255,
                .broadcast_schedule_id = 0x1234,
                .broadcast_interval_offset = rnd(&rng_sched, 1020),
                .broadcast_interval = 1020,
                .bt_rx_timestamp = ts(),
            };
            fhss_config(&c, false, true);
            CHECK(ns_fhss_ws_configuration_set(api, &c) == 0);
            api->synch_state_set(api, FHSS_SYNCHRONIZED, 0xabcd);
            CHECK(ns_fhss_ws_set_parent(api, parent_mac, &bt, true) == 0);
            printf("t%" PRIu64 " synchronized\n", now_us);
            break;
        }
        case S_UNSYNCHRONIZED:
            api->synch_state_set(api, FHSS_UNSYNCHRONIZED, 0xabcd);
            printf("t%" PRIu64 " unsynchronized\n", now_us);
            break;
    }
}

/* ---------------------------------------------------------- the schedule */

enum { A_QUEUE, A_READ, A_EF, A_STATE, A_COUNT };

/* The next scheduled instant after t, mostly short gaps. State changes fall
 * on a 50 µs boundary, frames and reads 25 µs off it: every callback timer
 * deadline lies on a 50 µs boundary (armed from a state change, from a timer
 * or in 50 µs slots), so a frame is never queued at the very µs of a poll,
 * whose order would be arbitrary. */
static uint64_t schedule_next(uint64_t t, int *action)
{
    uint64_t gap;
    int a = rnd(&rng_sched, 20);

    *action = a < 12 ? A_QUEUE : a < 16 ? A_READ : a < 17 ? A_EF : A_STATE;
    switch (rnd(&rng_sched, 6)) {
        case 0:
            gap = 2000000 + rnd(&rng_sched, 60000000);     // up to 62 s
            break;
        default:
            gap = 1 + rnd(&rng_sched, 400000);
            break;
    }
    t += gap;
    t -= t % 50;
    if (*action == A_QUEUE || *action == A_READ) {
        t += 25;
    }
    return t;
}

static void schedule_run(int action)
{
    switch (action) {
        case A_QUEUE:
            if (queue < 20) {
                queue++;
                printf("t%" PRIu64 " queued q=%u\n", now_us, queue);
                // As mcps_sap_pd_req_queue_write()
                if (api->tx_queued) {
                    api->tx_queued(api);
                }
            }
            break;
        case A_READ:
            printf("t%" PRIu64 " read q=%u\n", now_us, queue);
            break;
        case A_EF:
            api->synch_state_set(api, FHSS_EXPEDITED_FORWARDING, 0xabcd);
            printf("t%" PRIu64 " expedited forwarding\n", now_us);
            break;
        case A_STATE:
            // Mostly back to the idle state, where the poll runs
            state_enter(state == S_IDLE ? (int)(1 + rnd(&rng_sched, S_COUNT - 1)) : S_IDLE);
            break;
    }
}

/* ------------------------------------------------------------- run loop */

int main(void)
{
    const uint64_t end_us = 20ULL * 60 * 1000000;
    uint64_t sched_us, idle_us = 0;
    int action;
    fhss_ws_configuration_t c;

    now_us = 1000;
    fhss_config(&c, true, false);
    api = ns_fhss_ws_create(&c, &fake_fhss_timer);
    CHECK(api != NULL);
    api->init_callbacks(api, &mac_callbacks);
    ns_fhss_set_neighbor_info_fp(api, get_neighbor);
#ifdef NS_FHSS_POLL_SKIP_IDLE
    CHECK(api->tx_queued != NULL);
#else
    CHECK(api->tx_queued == NULL);
#endif
    state_enter(S_IDLE);

    sched_us = schedule_next(now_us, &action);
    while (now_us < end_us) {
        // Earliest of the poll timer, the FHSS timers and the schedule; at
        // the same µs, in that order
        uint64_t t = sched_us;
        int which = -2;
        if (poll_timer_armed && poll_timer_deadline <= t) {
            t = poll_timer_deadline;
            which = -1;
        }
        for (int i = 0; i < FHSS_TIMERS; i++) {
            if (ftimers[i].fn && ftimers[i].armed && ftimers[i].deadline < t) {
                t = ftimers[i].deadline;
                which = i;
            }
        }
        CHECK(t >= now_us);
        if (t >= end_us) {
            if (state == S_IDLE) {
                idle_us += end_us - now_us;
            }
            break;
        }
        if (state == S_IDLE) {
            idle_us += t - now_us;
        }
        now_us = t;
        if (which == -1) {
            poll_timer_armed = false;
            poll_timer_fires++;
            if (state == S_IDLE) {
                idle_fires++;
            }
            poll_timer_fn(1, 0);
        } else if (which >= 0) {
            ftimers[which].armed = false;
            ftimers[which].fn(ftimer_api, 0);
        } else {
            schedule_run(action);
            sched_us = schedule_next(now_us, &action);
        }
    }
    CHECK(ns_fhss_delete(api) == 0);

    fprintf(stderr, "fhss_poll: %lu polls, %lu poll timer wakes (%lu in %" PRIu64 " s of idle state)\n",
            polls, poll_timer_fires, idle_fires, idle_us / 1000000);
    CHECK(idle_us > end_us / 4);
#ifdef NS_FHSS_POLL_SKIP_IDLE
    // Wakes in the idle state only for frames: far below the periodic 100/s
    CHECK(idle_fires < idle_us / 1000000 * 10);
#else
    // The periodic 100/s, except where an earlier state's broadcast schedule
    // still runs (a fixed-channel configuration does not stop it)
    CHECK(idle_fires >= idle_us / 10000 / 2);
#endif
    return 0;
}
