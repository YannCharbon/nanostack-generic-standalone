/*
 * Stub port for the cross-build check: every function of the porting API
 * (include/ns_port/), doing as little as possible. It is linked, never run.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stddef.h>

#include "ns_port/ns_port.h"
#include "nanostack/mac/fhss_config.h"

static volatile int critical_depth;
static volatile int events_pending;

void platform_critical_init(void) { critical_depth = 0; }
void platform_enter_critical(void) { critical_depth++; }
void platform_exit_critical(void) { critical_depth--; }

#define STUB_TIMERS 16
static void (*timer_cb[STUB_TIMERS])(int, uint16_t);

int eventOS_callback_timer_register(void (*cb)(int, uint16_t))
{
    for (int i = 0; i < STUB_TIMERS; i++) {
        if (!timer_cb[i]) {
            timer_cb[i] = cb;
            return i;
        }
    }
    return -1;
}

int eventOS_callback_timer_unregister(int id)
{
    if (id < 0 || id >= STUB_TIMERS)
        return -1;
    timer_cb[id] = NULL;
    return 0;
}

int eventOS_callback_timer_start(int id, uint16_t slots) { (void)slots; return id < 0 ? -1 : 0; }
int eventOS_callback_timer_stop(int id) { return id < 0 ? -1 : 0; }

void ns_port_init(void) { events_pending = 0; }
void eventOS_scheduler_signal(void) { events_pending = 1; }

static void (*poll_hook)(void);
void ns_port_add_poll_hook(void (*hook)(void)) { poll_hook = hook; }

void ns_port_run(void)
{
    for (;;) {
        if (poll_hook)
            poll_hook();
        events_pending = 0;
        eventOS_scheduler_run_until_idle();
    }
}

/* Not random: a stub. A real port must use a TRNG (ns_port/random.h). */
void ns_port_random_bytes(void *buf, size_t n)
{
    static uint32_t x = 0x12345678;
    uint8_t *p = buf;

    while (n--) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *p++ = (uint8_t)x;
    }
}

static uint32_t now_us;
uint32_t ns_port_time_us(void) { return now_us; }
uint64_t ns_port_time_wall_s(void) { return 0; }

static int fhss_start(uint32_t us, void (*cb)(const fhss_api_t *, uint16_t), const fhss_api_t *api) { return 0; }
static int fhss_stop(void (*cb)(const fhss_api_t *, uint16_t), const fhss_api_t *api) { return 0; }
static uint32_t fhss_remaining(void (*cb)(const fhss_api_t *, uint16_t), const fhss_api_t *api) { return 0; }
static uint32_t fhss_timestamp(const fhss_api_t *api) { return ns_port_time_us(); }

const struct fhss_timer ns_port_fhss_timer = {
    .fhss_timer_start = fhss_start,
    .fhss_timer_stop = fhss_stop,
    .fhss_get_remaining_slots = fhss_remaining,
    .fhss_get_timestamp = fhss_timestamp,
    .fhss_resolution_divider = 1,
};

void ns_port_trace_write(const char *line) { (void)line; }
