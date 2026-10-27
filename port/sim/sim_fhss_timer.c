/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: FHSS platform timer (ns_port/fhss_timer.h) on virtual
 * time. One timeout per distinct callback, as in the POSIX port; the ID that
 * orders same-time timers is the order in which the callbacks first appeared.
 */
#include <stdlib.h>

#include "common/fhss_timer.h"
#include "common/log.h"
#include "common/utils.h"
#include "ns_port_sim.h"
#include "sim_internal.h"

struct sim_fhss_timer {
    bool armed;
    uint64_t deadline;
    const fhss_api_t *arg;
    void (*fn)(const fhss_api_t *api, uint16_t);
};

#define SIM_FHSS_TIMERS_MAX 8

static struct sim_fhss_timer fhss_timers[SIM_FHSS_TIMERS_MAX];
static int fhss_timers_len;

unsigned long g_sim_zero_starts_fhss;

static struct sim_fhss_timer *fhss_find(void (*callback)(const fhss_api_t *api, uint16_t))
{
    int i;

    for (i = 0; i < fhss_timers_len; i++)
        if (fhss_timers[i].fn == callback)
            return &fhss_timers[i];
    return NULL;
}

static int fhss_timer_start(uint32_t slots_us, void (*callback)(const fhss_api_t *api, uint16_t), const fhss_api_t *api)
{
    struct sim_fhss_timer *item = fhss_find(callback);

    if (!item) {
        FATAL_ON(fhss_timers_len >= SIM_FHSS_TIMERS_MAX, 2, "too many FHSS timers");
        item = &fhss_timers[fhss_timers_len++];
        item->fn = callback;
        item->arg = api;
    }
    // A zero length disarms, as timerfd_settime() does in the POSIX port.
    if (!slots_us) {
        g_sim_zero_starts_fhss++;
        item->armed = false;
        return 0;
    }
    item->armed = true;
    item->deadline = ns_port_sim_now_us() + slots_us;
    return 0;
}

static int fhss_timer_stop(void (*callback)(const fhss_api_t *api, uint16_t), const fhss_api_t *api)
{
    struct sim_fhss_timer *item = fhss_find(callback);

    if (!item)
        return -1;
    item->armed = false;
    return 0;
}

static uint32_t fhss_get_remaining_slots(void (*callback)(const fhss_api_t *api, uint16_t), const fhss_api_t *api)
{
    struct sim_fhss_timer *item = fhss_find(callback);
    uint64_t now = ns_port_sim_now_us();

    if (!item)
        return -1; // as the POSIX port
    if (!item->armed || item->deadline <= now)
        return 0;
    return min(item->deadline - now, (uint64_t)UINT32_MAX);
}

static uint32_t fhss_get_timestamp(const fhss_api_t *api)
{
    return (uint32_t)ns_port_sim_now_us();
}

const struct fhss_timer ns_port_fhss_timer = {
    .fhss_timer_start         = fhss_timer_start,
    .fhss_timer_stop          = fhss_timer_stop,
    .fhss_get_remaining_slots = fhss_get_remaining_slots,
    .fhss_get_timestamp       = fhss_get_timestamp,
    .fhss_resolution_divider  = 1
};

static int fhss_first_due(uint64_t now)
{
    int i, best = -1;

    for (i = 0; i < fhss_timers_len; i++) {
        if (!fhss_timers[i].armed || fhss_timers[i].deadline > now)
            continue;
        if (best < 0 || fhss_timers[i].deadline < fhss_timers[best].deadline)
            best = i;
    }
    return best;
}

bool sim_fhss_due(uint64_t now)
{
    return fhss_first_due(now) >= 0;
}

void sim_fhss_fire_due(uint64_t now)
{
    int i;

    while ((i = fhss_first_due(now)) >= 0) {
        fhss_timers[i].armed = false;
        fhss_timers[i].fn(fhss_timers[i].arg, 0);
    }
}

uint64_t sim_fhss_next(void)
{
    uint64_t next = NSIM_NEVER;
    int i;

    for (i = 0; i < fhss_timers_len; i++)
        if (fhss_timers[i].armed && fhss_timers[i].deadline < next)
            next = fhss_timers[i].deadline;
    return next;
}
