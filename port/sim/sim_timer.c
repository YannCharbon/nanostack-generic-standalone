/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: callback timers (ns_port/timer.h) and the POSIX port's
 * extras (common/os_timer.h) on virtual time. A timer is an absolute
 * deadline in µs, or unarmed; ns_port_run() fires the due ones.
 */
#include <stdlib.h>
#include <string.h>

#include "common/os_timer.h"
#include "common/log.h"
#include "ns_port_sim.h"
#include "sim_internal.h"

struct sim_timer {
    bool used;
    bool armed;
    uint64_t deadline;
    void (*fn)(int, uint16_t);
};

// Indexed by timer ID
static struct sim_timer *timers;
static int timers_len;

unsigned long g_sim_zero_starts_timer;

static struct sim_timer *timer_get(int ns_timer_id)
{
    if (ns_timer_id < 0 || ns_timer_id >= timers_len || !timers[ns_timer_id].used) {
        BUG("unknown callback timer %d", ns_timer_id);
        return NULL;
    }
    return &timers[ns_timer_id];
}

int eventOS_callback_timer_register(void (*timer_interrupt_handler)(int, uint16_t))
{
    int id;

    // Lowest free ID, as the POSIX port
    for (id = 0; id < timers_len; id++)
        if (!timers[id].used)
            break;
    if (id == timers_len) {
        timers = realloc(timers, (timers_len + 1) * sizeof(*timers));
        FATAL_ON(!timers, 2, "realloc: %m");
        timers_len++;
    }
    memset(&timers[id], 0, sizeof(timers[id]));
    timers[id].used = true;
    timers[id].fn = timer_interrupt_handler;
    return id;
}

int eventOS_callback_timer_unregister(int ns_timer_id)
{
    struct sim_timer *t = timer_get(ns_timer_id);

    memset(t, 0, sizeof(*t));
    return ns_timer_id;
}

// A zero length disarms, as timerfd_settime() does in the POSIX port.
static void timer_arm_us(int ns_timer_id, uint64_t us)
{
    struct sim_timer *t = timer_get(ns_timer_id);

    if (!us) {
        g_sim_zero_starts_timer++;
        t->armed = false;
        return;
    }
    t->armed = true;
    t->deadline = ns_port_sim_now_us() + us;
}

int eventOS_callback_timer_start(int ns_timer_id, uint16_t slots)
{
    timer_arm_us(ns_timer_id, (uint64_t)slots * 50);
    return 0;
}

int eventOS_callback_timer_stop(int ns_timer_id)
{
    timer_get(ns_timer_id)->armed = false;
    return 0;
}

int eventOS_callback_timer_start_sec(int ns_timer_id, int sec)
{
    FATAL_ON(sec < 0, 2, "timer %d: negative delay", ns_timer_id);
    timer_arm_us(ns_timer_id, (uint64_t)sec * 1000000);
    return 0;
}

int eventOS_callback_timer_start_usec(int ns_timer_id, int usec)
{
    FATAL_ON(usec < 0, 2, "timer %d: negative delay", ns_timer_id);
    timer_arm_us(ns_timer_id, usec);
    return 0;
}

// 1 when unarmed or due. The POSIX port's version also returns 1 when less
// than a second is left (it only looks at tv_sec); nothing calls it.
int eventOS_callback_timer_expired(int ns_timer_id)
{
    struct sim_timer *t = timer_get(ns_timer_id);

    return !t->armed || t->deadline <= ns_port_sim_now_us();
}

int eventOS_callback_timer_expired_usec(int ns_timer_id)
{
    return eventOS_callback_timer_expired(ns_timer_id);
}

// Earliest due timer, lowest ID first among equal deadlines; -1 if none.
static int timer_first_due(uint64_t now)
{
    int i, best = -1;

    for (i = 0; i < timers_len; i++) {
        if (!timers[i].used || !timers[i].armed || timers[i].deadline > now)
            continue;
        if (best < 0 || timers[i].deadline < timers[best].deadline)
            best = i;
    }
    return best;
}

bool sim_timer_due(uint64_t now)
{
    return timer_first_due(now) >= 0;
}

// Callbacks may register, re-arm or unregister timers: look again after each.
// A timer armed by a callback is never due at now (zero length disarms).
void sim_timer_fire_due(uint64_t now)
{
    void (*fn)(int, uint16_t);
    int id;

    while ((id = timer_first_due(now)) >= 0) {
        timers[id].armed = false;
        fn = timers[id].fn;
        fn(id, 0);
    }
}

uint64_t sim_timer_next(void)
{
    uint64_t next = NSIM_NEVER;
    int i;

    for (i = 0; i < timers_len; i++)
        if (timers[i].used && timers[i].armed && timers[i].deadline < next)
            next = timers[i].deadline;
    return next;
}
