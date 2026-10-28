/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the event loop's timers, periodic tick or with idle-tick skipping
 * (docs/PORTING.md, "Idle-tick skipping"). Built three times from this
 * file: with the periodic tick, idle-tick skipping with µs timers (NS_PORT_TIMER_USEC)
 * and idle-tick skipping with 50 µs slots only, on a fake port: a virtual µs clock and
 * callback timers with absolute deadlines, as the simulation port has.
 *
 * Each program runs the same pseudo-random schedule: one-shot and periodic
 * event timers requested at arbitrary instants (also at tick boundaries and
 * from event handlers), cancellations, long idle gaps (longer than the tick
 * timer's 3.28 s range) and a 32-bit clock wrap. It prints one line per
 * delivered event ("<µs> <type> <id>"); run.sh checks that the periodic and
 * the µs idle-tick skipping programs print the same lines (with 50 µs slots, a tick may
 * land up to one slot late, which can reorder it against the schedule's other
 * actions). The idle-tick skipping programs also check their own invariants:
 * no tick timer while no event timer is pending, far fewer tick wakes, and
 * eventOS_event_timer_shortest_active_timer().
 *
 *   tests/eventloop/run.sh [build dir]
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack-event-loop/eventOS_event_timer.h"
#include "nanostack-event-loop/eventOS_scheduler.h"
#include "nanostack-event-loop/eventOS_callback_timer.h"
#include "ns_port/critical.h"
#include "ns_port/time.h"

// nanostack/net_interface.h, which needs the whole stack's headers
#define ARM_LIB_TASKLET_INIT_EVENT 0

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s (t=%" PRIu64 " us)\n", __FILE__, __LINE__, #cond, now_us); \
            exit(1); \
        } \
    } while (0)

/* ------------------------------------------------------------ fake port */

static uint64_t now_us;
// The 32-bit clock starts near its wrap, so every run crosses it
#define CLOCK32_OFFSET_US (UINT32_MAX - 20000000u)

uint32_t ns_port_time_us(void)
{
    return (uint32_t)(now_us + CLOCK32_OFFSET_US);
}

uint64_t ns_port_time_wall_s(void)
{
    return 0;
}

static int critical_depth;
void platform_enter_critical(void) { critical_depth++; }
void platform_exit_critical(void) { CHECK(critical_depth > 0); critical_depth--; }
void eventOS_scheduler_signal(void) { }

#define TIMERS 4
static struct {
    bool used, armed;
    uint64_t deadline;
    void (*fn)(int, uint16_t);
} timers[TIMERS];
static unsigned long timer_fires[TIMERS];

int eventOS_callback_timer_register(void (*fn)(int, uint16_t))
{
    for (int i = 0; i < TIMERS; i++) {
        if (!timers[i].used) {
            timers[i].used = true;
            timers[i].fn = fn;
            return i;
        }
    }
    return -1;
}

int eventOS_callback_timer_unregister(int id)
{
    memset(&timers[id], 0, sizeof(timers[id]));
    return id;
}

// Zero slots disarm, as on the simulation and POSIX ports
int eventOS_callback_timer_start(int id, uint16_t slots)
{
    CHECK(id >= 0 && id < TIMERS && timers[id].used);
    timers[id].armed = slots != 0;
    timers[id].deadline = now_us + (uint64_t)slots * 50;
    return 0;
}

#ifdef NS_PORT_TIMER_USEC
int eventOS_callback_timer_start_usec(int id, int usec)
{
    CHECK(id >= 0 && id < TIMERS && timers[id].used && usec >= 0);
    timers[id].armed = usec != 0;
    timers[id].deadline = now_us + usec;
    return 0;
}
#endif

int eventOS_callback_timer_stop(int id)
{
    timers[id].armed = false;
    return 0;
}

/* ----------------------------------------------------------- the schedule */

static uint64_t rng = 12345;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng >> 33) % n;
}

static int8_t tasklet;
static unsigned long delivered;
#define PERIODIC 8
static arm_event_storage_t *periodic[PERIODIC];

enum { EV_ONE_SHOT = 1, EV_CHAIN = 2, EV_PERIODIC = 3 };

static void handler(arm_event_t *ev)
{
    if (ev->event_type == ARM_LIB_TASKLET_INIT_EVENT)
        return;
    delivered++;
    printf("%" PRIu64 " %u %u\n", now_us, ev->event_type, ev->event_id);
#if defined NS_EVENTLOOP_SKIP_IDLE_TICKS && !defined NS_PORT_TIMER_USEC
    // 50 µs slots: a tick lands up to one slot after its grid point
    CHECK(now_us % 10000 < 50);
#else
    CHECK(now_us % 10000 == 0);
#endif
    // A handler that asks for another timer, as the stack's modules do
    if (ev->event_type == EV_CHAIN && ev->event_id < 200)
        CHECK(!eventOS_event_timer_request(ev->event_id + 1, EV_CHAIN, tasklet, 10 + rnd(300)));
}

static void request_some(void)
{
    uint8_t id = rnd(250);

    switch (rnd(6)) {
    case 0:
    case 1:
        CHECK(!eventOS_event_timer_request(id, EV_ONE_SHOT, tasklet, rnd(5000)));
        break;
    case 2:
        CHECK(!eventOS_event_timer_request(id % 100, EV_CHAIN, tasklet, rnd(400)));
        break;
    case 3: {
        int slot = rnd(PERIODIC);
        const arm_event_t ev = {
            .receiver = tasklet, .event_type = EV_PERIODIC, .event_id = slot,
            .priority = ARM_LIB_MED_PRIORITY_EVENT,
        };
        if (periodic[slot])
            eventOS_cancel(periodic[slot]);
        periodic[slot] = eventOS_event_timer_request_every(&ev, 1 + rnd(150));
        CHECK(periodic[slot]);
        break;
    }
    case 4: {
        int slot = rnd(PERIODIC);
        if (periodic[slot]) {
            eventOS_cancel(periodic[slot]);
            periodic[slot] = NULL;
        }
        break;
    }
    default:
        // Very long one-shots: beyond the tick timer's 16-bit range
        CHECK(!eventOS_event_timer_request(id, EV_ONE_SHOT, tasklet, 3000 + rnd(9000)));
        break;
    }
}

static uint64_t next_deadline(void)
{
    uint64_t next = UINT64_MAX;

    for (int i = 0; i < TIMERS; i++)
        if (timers[i].used && timers[i].armed && timers[i].deadline < next)
            next = timers[i].deadline;
    return next;
}

static int tick_id = -1;

// One instant: the callback timers due, then the events, as ns_port_run()
static void run_instant(void)
{
    bool again = true;

    while (again) {
        again = false;
        for (int i = 0; i < TIMERS; i++) {
            if (timers[i].used && timers[i].armed && timers[i].deadline <= now_us) {
                CHECK(timers[i].deadline == now_us);
                timers[i].armed = false;
                timer_fires[i]++;
                timers[i].fn(i, 0);
                again = true;
            }
        }
        while (eventOS_scheduler_dispatch_event())
            again = true;
    }
    CHECK(critical_depth == 0);
}

#ifdef NS_EVENTLOOP_SKIP_IDLE_TICKS
// Nothing pending: the tick timer is off
static void check_tick_off_when_idle(void)
{
    if (eventOS_event_timer_shortest_active_timer() == 0)
        CHECK(!timers[tick_id].armed);
}
#endif

int main(void)
{
    uint64_t end_us = 900ULL * 1000000;   // 900 s, across the 32-bit wrap at 20 s
    uint64_t next_action = 0;

    eventOS_scheduler_init();
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].used)
            tick_id = i;
    CHECK(tick_id >= 0);
    tasklet = eventOS_event_handler_create(handler, ARM_LIB_TASKLET_INIT_EVENT);
    CHECK(tasklet >= 0);
    run_instant();
#ifdef NS_EVENTLOOP_SKIP_IDLE_TICKS
    CHECK(eventOS_event_timer_shortest_active_timer() == 0);
    CHECK(!timers[tick_id].armed);
    // A request arms the tick for the grid point it expires on: 25 ms -> 3 ticks
    CHECK(!eventOS_event_timer_request(0, EV_ONE_SHOT, tasklet, 25));
    CHECK(timers[tick_id].armed && timers[tick_id].deadline == 30000);
    CHECK(eventOS_event_timer_shortest_active_timer() == 30);
    // Requested between two slots: on the grid with µs, else the next slot
    now_us = 1234;
    CHECK(!eventOS_event_timer_request(1, EV_ONE_SHOT, tasklet, 0));
#ifdef NS_PORT_TIMER_USEC
    CHECK(timers[tick_id].deadline == 20000);
#else
    CHECK(timers[tick_id].deadline == 20034);
#endif
    run_instant();
#else
    CHECK(!eventOS_event_timer_request(0, EV_ONE_SHOT, tasklet, 25));
    now_us = 1234;
    CHECK(!eventOS_event_timer_request(1, EV_ONE_SHOT, tasklet, 0));
    run_instant();
#endif

    while (now_us < end_us) {
        uint64_t next = next_deadline();

        if (next_action <= next) {
            now_us = next_action;
            run_instant();      // the tick (if due now) first, as on a port
            request_some();
            if (!rnd(4))
                request_some();
            run_instant();
            // Gaps from 0 (same instant) to beyond the tick timer's range,
            // half of them on the 10 ms grid
            switch (rnd(8)) {
            case 0: next_action += 0; break;
            case 1: next_action += 10000 * (1 + rnd(30)); break;
            case 2: next_action += 1 + rnd(9999); break;
            case 3: next_action += 4000000 + rnd(3000000); break;
            default: next_action += rnd(200000); break;
            }
            if (!rnd(50))
                next_action += 20000000;   // 20 s idle
        } else {
            now_us = next;
            run_instant();
        }
#ifdef NS_EVENTLOOP_SKIP_IDLE_TICKS
        check_tick_off_when_idle();
#endif
    }
    fprintf(stderr, "%s: %lu events delivered, %lu tick timer callbacks in %" PRIu64 " s\n",
#if defined NS_EVENTLOOP_SKIP_IDLE_TICKS && defined NS_PORT_TIMER_USEC
            "idle-tick skipping",
#elif defined NS_EVENTLOOP_SKIP_IDLE_TICKS
            "idle-tick skipping, 50 us slots",
#else
            "periodic",
#endif
            delivered, timer_fires[tick_id], end_us / 1000000);
    CHECK(delivered > 1000);
#ifdef NS_EVENTLOOP_SKIP_IDLE_TICKS
    // A periodic tick would fire 90 000 times
    CHECK(timer_fires[tick_id] < end_us / 10000 / 2);
#else
    CHECK(timer_fires[tick_id] >= end_us / 10000 - 1);
#endif
    return 0;
}
