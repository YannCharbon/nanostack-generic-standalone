/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: what each wake did, for the conductor's power ledger
 * ("xc" in ns_port_sim.h; mesh-emulator-orchestrator docs/POWER_MODEL.md).
 *
 * Only active when the welcome has NSIM_WELCOME_POWER. A timer callback is
 * given a cause from its symbol name, looked up once per callback in the
 * program's symbol table (the stack's timer callbacks are static functions,
 * so dladdr() cannot name them). Without libbacktrace, or for a stripped
 * program, every callback timer is NSIM_CAUSE_OTHER.
 */
#include <stdint.h>
#include <string.h>
#ifdef HAVE_BACKTRACE
#include <backtrace.h>
#endif

#include "ns_port_sim.h"
#include "sim_internal.h"

bool g_sim_power;

static struct nsim_wake wake = { .magic = { 'x', 'c' } };

// Callbacks seen so far and their cause
#define SIM_POWER_FNS_MAX 64
static struct {
    const void *fn;
    enum nsim_cause cause;
} fns[SIM_POWER_FNS_MAX];
static int fns_len;

// First match wins. Substrings, except where marked as a prefix.
static const struct {
    const char *pattern;
    bool prefix;
    enum nsim_cause cause;
} rules[] = {
    { "tick_timer_eventOS_callback", false, NSIM_CAUSE_TICK },     // system_timer.c
    { "protocol_timer_interrupt",    false, NSIM_CAUSE_PROTOCOL }, // protocol_timer.c
    { "fhss",                        false, NSIM_CAUSE_FHSS },     // fhss_ws.c
    { "_rf_",                        true,  NSIM_CAUSE_RADIO },    // the emulated radio driver
    { "mac",                         false, NSIM_CAUSE_MAC },      // mac_mlme.c: timer_mac_interrupt, mac_*_timer_cb
};

#ifdef HAVE_BACKTRACE
static void sim_power_syminfo_cb(void *data, uintptr_t pc, const char *symname,
                                 uintptr_t symval, uintptr_t symsize)
{
    *(const char **)data = symname;
}

static void sim_power_error_cb(void *data, const char *msg, int errnum)
{
}
#endif

static const char *sim_power_symbol(const void *fn)
{
    const char *name = NULL;
#ifdef HAVE_BACKTRACE
    static struct backtrace_state *state;
    static bool tried;

    if (!tried) {
        tried = true;
        state = backtrace_create_state(NULL, 0, sim_power_error_cb, NULL);
    }
    if (state)
        backtrace_syminfo(state, (uintptr_t)fn, sim_power_syminfo_cb, sim_power_error_cb, &name);
#endif
    return name;
}

static enum nsim_cause sim_power_classify(const void *fn)
{
    const char *name = sim_power_symbol(fn);
    int i;

    if (!name)
        return NSIM_CAUSE_OTHER;
    for (i = 0; i < (int)(sizeof(rules) / sizeof(rules[0])); i++) {
        if (rules[i].prefix ? !strncmp(name, rules[i].pattern, strlen(rules[i].pattern))
                            : !!strstr(name, rules[i].pattern))
            return rules[i].cause;
    }
    return NSIM_CAUSE_OTHER;
}

static enum nsim_cause sim_power_cause(const void *fn)
{
    enum nsim_cause cause;
    int i;

    for (i = 0; i < fns_len; i++)
        if (fns[i].fn == fn)
            return fns[i].cause;
    cause = sim_power_classify(fn);
    if (fns_len < SIM_POWER_FNS_MAX) {
        fns[fns_len].fn = fn;
        fns[fns_len].cause = cause;
        fns_len++;
    }
    return cause;
}

static void sim_power_inc(uint16_t *counter, unsigned int n)
{
    *counter = (*counter + n > UINT16_MAX) ? UINT16_MAX : *counter + n;
}

void sim_power_timer_fired(const void *fn, bool fhss)
{
    sim_power_inc(&wake.timers[fhss ? NSIM_CAUSE_FHSS : sim_power_cause(fn)], 1);
}

void sim_power_events(unsigned int n)
{
    sim_power_inc(&wake.events, n);
}

void sim_power_take(struct nsim_wake *out)
{
    *out = wake;
    memset(&wake, 0, sizeof(wake));
    memcpy(wake.magic, "xc", 2);
}

bool ns_port_sim_power(void)
{
    return g_sim_power;
}
