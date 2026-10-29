/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the idle-tick skipping of the protocol timers (docs/PORTING.md,
 * "Idle-tick skipping of the protocol timers"). The stack's own
 * protocol_timer.c (the 50 ms protocol timer), protocol_core.c (the 100 ms
 * core timer and its 1 s slow timers, the catch-up and re-check hooks) and
 * event loop run on a fake port (a virtual µs clock, callback timers with
 * absolute deadlines, a run loop like the simulation port's) with fake
 * modules in place of the stack's. Built three times from this file: with
 * the periodic timers, with skipping and µs timers (NS_PORT_TIMER_USEC), and
 * with skipping and 50 µs slots only (the FG25 port).
 *
 * The fake modules are hooked where protocol_core.c calls the stack's
 * modules (no interface exists, so only the global ones are called):
 *   F  rpl_control_fast_timer()       countdowns in 100 ms ticks
 *   U  icmpv6_radv_timer()            counted down by one per call, whatever
 *                                     the ticks (a module that says "every
 *                                     tick" while it runs)
 *   S  ws_pae_controller_slow_timer() countdowns in seconds
 *   P  ipv6_destination_cache_timer() a 20 s period that keeps its phase,
 *                                     acting only while switched on (and
 *                                     due only then)
 * plus two users of protocol_timer_start() as MPL and TCP are (a 50 ms and a
 * 200 ms tick while they run). A countdown that runs out acts: it prints, and
 * may start another one (random values, drawn in the order of the actions).
 *
 * A schedule of events, posted at arbitrary instants as a radio driver would
 * (never within 1 ms after a 50 ms grid point, so that a slot-late timer of
 * the slots variant cannot reorder them), starts countdowns and the protocol
 * timer users, switches P, and reads every module's state and the core time
 * (protocol_core_monotonic_time), as the stack's code would between two
 * core runs. It includes long idle gaps, beyond the 16-bit slot range and the
 * core's longest wait, and the 32-bit clock wrap.
 *
 * Each program prints one line per action and per read, stamped with the
 * 50 ms grid point (actions) or the µs (reads). run.sh requires the three
 * outputs to be identical. The skipping programs also check that the
 * protocol timer woke far less often, and that no slow timer got more than
 * 255 seconds at once.
 *
 *   tests/protocol_timers/run.sh [build dir]
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack-event-loop/eventOS_scheduler.h"
#include "nanostack-event-loop/eventOS_callback_timer.h"
#include "ns_port/critical.h"
#include "ns_port/time.h"

#ifndef ARM_LIB_TASKLET_INIT_EVENT
#define ARM_LIB_TASKLET_INIT_EVENT 0
#endif
#define NONE UINT32_MAX

// protocol_core.c and protocol_timer.c, without their headers (which need
// the whole stack's)
void protocol_core_init(void);
void protocol_timer_start(int id, void (*fptr)(uint16_t), uint32_t time_ms);
extern uint32_t protocol_core_monotonic_time;
enum { PROTOCOL_TIMER_STACK_TIM, PROTOCOL_TIMER_TCP_TIM, PROTOCOL_TIMER_MULTICAST_TIM };

static uint64_t now_us;

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s (t=%" PRIu64 " us)\n", __FILE__, __LINE__, #cond, now_us); \
            exit(1); \
        } \
    } while (0)

/* ------------------------------------------------------------ fake port */

// The 32-bit clock starts near its wrap, so every run crosses it
#define CLOCK32_OFFSET_US (UINT32_MAX - 30000000u)

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

/* -------------------------------------------- stack functions not called */

void mbed_tracef(uint8_t dlevel, const char *grp, const char *fmt, ...)
{
    (void)dlevel;
    (void)grp;
    (void)fmt;
}

// Per-interface modules and the rest of protocol_core.c: no interface
// exists, so none of them is called
#define NOT_CALLED(name) void name(void) { fprintf(stderr, "%s called\n", #name); abort(); }
NOT_CALLED(addr_delete_entry) NOT_CALLED(addr_fast_timer) NOT_CALLED(addr_fast_timer_next)
NOT_CALLED(addr_is_assigned_to_interface) NOT_CALLED(addr_max_slaac_entries_set)
NOT_CALLED(addr_slow_timer) NOT_CALLED(addr_slow_timer_next) NOT_CALLED(bitsequal)
NOT_CALLED(buffer_free) NOT_CALLED(buffer_free_route) NOT_CALLED(common_read_16_bit)
NOT_CALLED(etx_cache_timer) NOT_CALLED(etx_cache_timer_next) NOT_CALLED(icmpv6_radv_init)
NOT_CALLED(icmpv6_stop_router_advertisements) NOT_CALLED(ipv6_buffer_route)
NOT_CALLED(ipv6_neighbour_cache_fast_timer) NOT_CALLED(ipv6_neighbour_cache_fast_timer_next)
NOT_CALLED(ipv6_neighbour_cache_flush) NOT_CALLED(ipv6_neighbour_cache_init)
NOT_CALLED(ipv6_neighbour_cache_print) NOT_CALLED(ipv6_neighbour_cache_slow_timer)
NOT_CALLED(ipv6_neighbour_cache_slow_timer_next) NOT_CALLED(ipv6_route_table_set_max_entries)
NOT_CALLED(ipv6_route_table_source_invalidated_reset) NOT_CALLED(ipv6_route_table_source_was_invalidated)
NOT_CALLED(lowpan_adaptation_interface_free) NOT_CALLED(lowpan_adaptation_interface_init)
NOT_CALLED(lowpan_adaptation_interface_slow_timer) NOT_CALLED(lowpan_adaptation_interface_slow_timer_next)
NOT_CALLED(lowpan_context_list_free) NOT_CALLED(lowpan_context_timer) NOT_CALLED(lowpan_context_timer_next)
NOT_CALLED(mac_helper_mac64_set) NOT_CALLED(mac_helper_set_default_key_source)
NOT_CALLED(mac_neighbor_table_neighbor_timeout_next) NOT_CALLED(mac_neighbor_table_neighbor_timeout_update)
NOT_CALLED(mcps_data_confirm_handler) NOT_CALLED(mcps_data_indication_handler)
NOT_CALLED(mcps_purge_confirm_handler) NOT_CALLED(mld_fast_timer) NOT_CALLED(mld_fast_timer_next)
NOT_CALLED(mld_slow_timer) NOT_CALLED(mlme_confirm_handler) NOT_CALLED(mlme_indication_handler)
NOT_CALLED(nd_object_timer) NOT_CALLED(nd_object_timer_next) NOT_CALLED(rand_randomise_base)
NOT_CALLED(reassembly_interface_free) NOT_CALLED(reassembly_interface_init)
NOT_CALLED(rpl_control_remove_domain_from_interface) NOT_CALLED(socket_tx_buffer_event)
NOT_CALLED(socket_tx_buffer_event_and_free) NOT_CALLED(ws_common_fast_timer)
NOT_CALLED(ws_common_fast_timer_next) NOT_CALLED(ws_common_seconds_timer)
NOT_CALLED(ws_common_seconds_timer_next) NOT_CALLED(ws_common_state_machine)
char rfc7731_default_control_message_trickle_params[16];
char rfc7731_default_data_message_trickle_params[16];

/* ---------------------------------------------------------- fake modules */

static uint64_t rng_mod = 777;      // drawn by the modules' actions only
static uint64_t rng_sched = 12345;  // drawn by the schedule only
static uint32_t rnd(uint64_t *rng, uint32_t n)
{
    *rng = *rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(*rng >> 33) % n;
}

static uint64_t grid0_us;   // grid point 0 of the protocol timer
static void act(const char *fmt, ...)
{
    va_list ap;

    printf("g%" PRIu64 " ", (now_us - grid0_us) / 50000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

#define SLOTS 6
typedef struct {
    const char *name;
    uint32_t cd[SLOTS];     // 0: not running
} countdowns_t;

static countdowns_t mod_f = { "F" }, mod_s = { "S" };

static void countdowns_run(countdowns_t *m, uint32_t n)
{
    for (int i = 0; i < SLOTS; i++) {
        if (!m->cd[i]) {
            continue;
        }
        if (m->cd[i] > n) {
            m->cd[i] -= n;
            continue;
        }
        m->cd[i] = 0;
        act("%s %d fires", m->name, i);
        // Sometimes chains another one, as a module re-arming itself
        if (rnd(&rng_mod, 3) == 0) {
            m->cd[i] = 1 + rnd(&rng_mod, 40);
            act("%s %d again in %" PRIu32, m->name, i, m->cd[i]);
        }
    }
}

static uint32_t countdowns_next(const countdowns_t *m)
{
    uint32_t next = NONE;

    for (int i = 0; i < SLOTS; i++) {
        if (m->cd[i] && m->cd[i] < next) {
            next = m->cd[i];
        }
    }
    return next;
}

static uint32_t mod_u_calls;        // counted down by one per call
static bool mod_p_on;
static uint16_t mod_p_phase;        // seconds into the 20 s period
static uint32_t max_seconds;

// F
void rpl_control_fast_timer(uint16_t ticks) { CHECK(ticks >= 1); countdowns_run(&mod_f, ticks); }
uint32_t rpl_control_fast_timer_next(void) { return countdowns_next(&mod_f); }

// U
void icmpv6_radv_timer(uint16_t ticks)
{
    CHECK(ticks >= 1);
    if (mod_u_calls && --mod_u_calls == 0) {
        act("U done");
    }
}
uint32_t icmpv6_radv_timer_next(void) { return mod_u_calls ? 1 : NONE; }

void ws_pae_controller_fast_timer(uint16_t ticks) { CHECK(ticks >= 1); }
uint32_t ws_pae_controller_fast_timer_next(void) { return NONE; }

// S
void ws_pae_controller_slow_timer(uint16_t seconds)
{
    CHECK(seconds >= 1);
    if (seconds > max_seconds) {
        max_seconds = seconds;
    }
    countdowns_run(&mod_s, seconds);
}
uint32_t ws_pae_controller_slow_timer_next(void) { return countdowns_next(&mod_s); }

// P: as ipv6_destination_cache_timer(), a period that keeps its phase
void ipv6_destination_cache_timer(uint8_t seconds)
{
    CHECK(seconds >= 1);
    mod_p_phase += seconds;
    while (mod_p_phase >= 20) {
        // Several periods at once only while off (the next one is due)
        CHECK(!mod_p_on || mod_p_phase < 40);
        mod_p_phase -= 20;
        if (mod_p_on) {
            act("P period");
        }
    }
}
uint32_t ipv6_destination_cache_timer_next(void) { return mod_p_on ? 20 - mod_p_phase : NONE; }

#define SLOW_IDLE(name, type) \
    void name(type seconds) { CHECK(seconds >= 1 && seconds <= 255); } \
    uint32_t name##_next(void) { return NONE; }
SLOW_IDLE(mpl_slow_timer, uint16_t)
SLOW_IDLE(rpl_control_slow_timer, uint16_t)
SLOW_IDLE(ipv6_route_table_ttl_update, uint16_t)
SLOW_IDLE(ipv6_frag_timer, uint8_t)
SLOW_IDLE(cipv6_frag_timer, uint16_t)
SLOW_IDLE(net_dns_timer_seconds, uint32_t)
uint32_t rpl_control_fast_timer_next(void);
uint32_t mld_slow_timer_next(unsigned calls) { CHECK(calls == 0); return NONE; }

// Users of protocol_timer_start(), as MPL (50 ms) and TCP (200 ms)
static uint32_t mpl_left, tcp_left;
static void mpl_tick(uint16_t ticks)
{
    act("mpl tick %u", ticks);
    if (--mpl_left) {
        protocol_timer_start(PROTOCOL_TIMER_MULTICAST_TIM, mpl_tick, 50);
    }
}
static void tcp_tick(uint16_t ticks)
{
    act("tcp tick %u", ticks);
    if (--tcp_left) {
        protocol_timer_start(PROTOCOL_TIMER_TCP_TIM, tcp_tick, 200);
    }
}

/* ---------------------------------------------------------- the schedule */

enum { A_READ, A_START_F, A_START_S, A_START_U, A_P, A_MPL, A_TCP, A_COUNT };

static void read_state(void)
{
    printf("t%" PRIu64 " read mono=%" PRIu32 " F", now_us, protocol_core_monotonic_time);
    for (int i = 0; i < SLOTS; i++) {
        printf(" %" PRIu32, mod_f.cd[i]);
    }
    printf(" S");
    for (int i = 0; i < SLOTS; i++) {
        printf(" %" PRIu32, mod_s.cd[i]);
    }
    printf(" U %" PRIu32 " P %u\n", mod_u_calls, mod_p_phase);
}

static void handler(arm_event_t *ev)
{
    int i = ev->event_id % SLOTS;
    uint32_t v = (uint32_t)ev->event_data;

    if (ev->event_type == ARM_LIB_TASKLET_INIT_EVENT) {
        return;
    }
    switch (ev->event_type - 1) {
        case A_READ:
            read_state();
            break;
        case A_START_F:
            mod_f.cd[i] = v;
            printf("t%" PRIu64 " F %d start %" PRIu32 "\n", now_us, i, v);
            break;
        case A_START_S:
            mod_s.cd[i] = v;
            printf("t%" PRIu64 " S %d start %" PRIu32 "\n", now_us, i, v);
            break;
        case A_START_U:
            mod_u_calls = v;
            printf("t%" PRIu64 " U start %" PRIu32 "\n", now_us, v);
            break;
        case A_P:
            mod_p_on = !mod_p_on;
            printf("t%" PRIu64 " P %s\n", now_us, mod_p_on ? "on" : "off");
            break;
        case A_MPL:
            if (!mpl_left) {
                mpl_left = v;
                protocol_timer_start(PROTOCOL_TIMER_MULTICAST_TIM, mpl_tick, 50);
                printf("t%" PRIu64 " mpl start %" PRIu32 "\n", now_us, v);
            }
            break;
        case A_TCP:
            if (!tcp_left) {
                tcp_left = v;
                protocol_timer_start(PROTOCOL_TIMER_TCP_TIM, tcp_tick, 200);
                printf("t%" PRIu64 " tcp start %" PRIu32 "\n", now_us, v);
            }
            break;
    }
}

static int8_t tasklet;

// The next scheduled instant after t: an arbitrary gap, mostly short,
// sometimes long, never within 1 ms after a protocol grid point
static uint64_t schedule_next(uint64_t t)
{
    uint64_t gap;

    switch (rnd(&rng_sched, 10)) {
        case 0:
            gap = 5000000 + rnd(&rng_sched, 400000000);    // up to 405 s
            break;
        case 1:
        case 2:
            gap = 1000000 + rnd(&rng_sched, 20000000);
            break;
        default:
            gap = 1 + rnd(&rng_sched, 1500000);
            break;
    }
    t += gap;
    if ((t - grid0_us) % 50000 < 1000) {
        t += 1000;
    }
    if ((t - grid0_us) % 50000 > 49000) {
        t += 2000;
    }
    return t;
}

static void schedule_post(void)
{
    int a = rnd(&rng_sched, A_COUNT + 2);
    arm_event_t ev = {
        .receiver = tasklet,
        .sender = 0,
        .event_type = 1 + (a >= A_COUNT ? A_READ : a),
        .event_id = rnd(&rng_sched, SLOTS),
        .priority = ARM_LIB_MED_PRIORITY_EVENT,
    };

    switch (ev.event_type - 1) {
        case A_START_F:
            ev.event_data = 1 + rnd(&rng_sched, rnd(&rng_sched, 4) ? 60 : 3000);
            break;
        case A_START_S:
            ev.event_data = 1 + rnd(&rng_sched, rnd(&rng_sched, 4) ? 30 : 900);
            break;
        case A_START_U:
            ev.event_data = 1 + rnd(&rng_sched, 25);
            break;
        case A_MPL:
            ev.event_data = 1 + rnd(&rng_sched, 30);
            break;
        case A_TCP:
            ev.event_data = 1 + rnd(&rng_sched, 10);
            break;
    }
    CHECK(eventOS_event_send(&ev) == 0);
}

/* ------------------------------------------------------------- run loop */

static bool fire_one_timer(void)
{
    int best = -1;

    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].armed && timers[i].deadline <= now_us &&
                (best < 0 || timers[i].deadline < timers[best].deadline)) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    timers[best].armed = false;
    timer_fires[best]++;
    timers[best].fn(best, 0);
    return true;
}

static uint64_t next_deadline(void)
{
    uint64_t next = UINT64_MAX;

    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].armed && timers[i].deadline < next) {
            next = timers[i].deadline;
        }
    }
    return next;
}

int main(void)
{
    // 6 hours of virtual time
    const uint64_t end_us = 6ULL * 3600 * 1000000;
    uint64_t sched_us;
    unsigned long reads = 0, wakes = 0;

    eventOS_scheduler_init();
    tasklet = eventOS_event_handler_create(handler, ARM_LIB_TASKLET_INIT_EVENT);
    CHECK(tasklet >= 0);
    now_us = 1234;
    grid0_us = now_us;
    protocol_core_init();
    CHECK(timers[0].used);   // the protocol timer took the first callback timer ID

    sched_us = schedule_next(now_us);
    while (now_us < end_us) {
        bool busy;
        do {
            busy = false;
            while (eventOS_scheduler_dispatch_event()) {
                busy = true;
            }
            if (fire_one_timer()) {
                busy = true;
            }
        } while (busy);
        uint64_t t = next_deadline();
        if (sched_us <= t) {
            now_us = sched_us;
            schedule_post();
            reads++;
            sched_us = schedule_next(now_us);
        } else {
            // A wake for a callback timer, whether it still fires (the
            // catch-up before an event may have moved it)
            CHECK(t != UINT64_MAX);
            now_us = t;
            wakes++;
        }
    }
    CHECK(critical_depth == 0);

    unsigned long fires = 0;
    for (int i = 0; i < TIMERS; i++) {
        fires += timer_fires[i];
    }
    fprintf(stderr, "protocol_timers: %lu timer wakes (%lu callbacks) in %" PRIu64 " s, %lu scheduled events, longest slow-timer catch-up %" PRIu32 " s\n",
            wakes, fires, end_us / 1000000, reads, max_seconds);
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    // Far fewer wakes than the periodic 20 per second
    CHECK(wakes < end_us / 1000000 * 2);
    CHECK(max_seconds > 1 && max_seconds <= 255);
#else
    CHECK(wakes >= end_us / 50000 - 1);
    CHECK(max_seconds == 1);
#endif
    return 0;
}
