/*
 * Copyright (c) 2014-2019, Pelion and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "nsconfig.h"
#include <stdint.h>
#include "mbed-client-libservice/ns_trace.h"
#include "ns_port/critical.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack-event-loop/eventOS_callback_timer.h"
#include "nwk_interface/protocol.h"
#include "nwk_interface/protocol_timer.h"
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
#include "ns_port/time.h"
#include "ns_port/timer.h"
#endif

#define TRACE_GROUP "ctim"

#define PROTOCOL_TIMER_INTERVAL 1000    // 50us units, so we use 50ms

NS_LARGE protocol_timer_t protocol_timer[PROTOCOL_TIMER_MAX];
int protocol_timer_id = -1;
bool protocol_tick_handle_busy = false;
static uint16_t  protocol_tick_update = 0;

#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
/*
 * Idle-tick skipping (docs/PORTING.md, "Idle-tick skipping of the protocol
 * timers"). The timers keep their 50 ms grid, which starts at
 * protocol_timer_init() as the periodic callback timer did, but the callback
 * timer is armed only for the grid point at which the earliest running timer
 * expires, and not at all when none runs. The grid points are counted from
 * ns_port_time_us(): by the callback timer when it fires (handed to the
 * protocol event as before), and by protocol_timer_catch_up() before a timer
 * is started, so that a new timer counts from the current grid point, as
 * with the periodic timer.
 */
#define PT_TICK_US      (PROTOCOL_TIMER_INTERVAL * 50)
#define PT_SLOT_US      50
#ifdef NS_PORT_TIMER_USEC
#define PT_MAX_US       (1UL << 30)
#else
// 16-bit slots: 3.28 s. A later expiry wakes once in between and re-arms.
#define PT_MAX_US       (0xFFFFUL * PT_SLOT_US)
#endif

uint32_t protocol_timer_ticks_applied;
static uint32_t pt_grid_us;     // ns_port_time_us() of the last grid point counted
static bool pt_in_cb;           // protocol_timer_cb() is running: it re-arms at its end
static void pt_catch_up(uint32_t now);
static void pt_rearm(uint32_t now);
#endif

int protocol_timer_init(void)
{
    uint8_t i;
    protocol_timer_id = eventOS_callback_timer_register(protocol_timer_interrupt);
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        protocol_timer[i].ticks = 0;
        protocol_timer[i].time_drifts = 0;
    }
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    // Grid point 0 is now; the callback timer starts with the first timer
    pt_grid_us = ns_port_time_us();
    protocol_timer_ticks_applied = 0;
#else
    if (protocol_timer_id >= 0) {
        eventOS_callback_timer_start(protocol_timer_id, PROTOCOL_TIMER_INTERVAL);
    }
#endif
    return protocol_timer_id;
}

// time is in milliseconds
void protocol_timer_start(protocol_timer_id_t id, void (*passed_fptr)(uint16_t), uint32_t time)
{
    //Check Overflow
    if (passed_fptr) {
        if (time > 0x12FFED) {
            time = 0xffff;
        }
        if (time >= 100) {
            time /= (1000 / 20);
            //time++;
        } else {
            time = 1;
        }
        platform_enter_critical();
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
        uint32_t now = ns_port_time_us();
        pt_catch_up(now);
#endif
        protocol_timer[id].ticks = (uint16_t) time;

        protocol_timer[id].orderedTime = (uint16_t) time;
        if (time > 1 && protocol_timer[id].time_drifts >= 50) {
            protocol_timer[id].ticks--;
            protocol_timer[id].time_drifts -= 50;
        }
        protocol_timer[id].fptr = passed_fptr;
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
        pt_rearm(now);
#endif
        platform_exit_critical();
    } else {
        tr_debug("Do Not use Null pointer for fptr!!!");
    }
}

void protocol_timer_stop(protocol_timer_id_t id)
{
    platform_enter_critical();
    protocol_timer[id].ticks = 0;
    protocol_timer[id].orderedTime = 0;
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    pt_rearm(ns_port_time_us());
#endif
    platform_exit_critical();
}



void protocol_timer_sleep_balance(uint32_t time_in_ms)
{
    uint8_t i;
    uint16_t ticks_module;
    uint16_t time_in50ms_ticks;
    uint16_t tick_update, tempTimer;

    ticks_module = (time_in_ms % 50);
    time_in50ms_ticks = (time_in_ms / 50);
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        if (protocol_timer[i].ticks) {

            tick_update = time_in50ms_ticks;
            protocol_timer[i].time_drifts += ticks_module;

            if (protocol_timer[i].time_drifts >= 50) {
                protocol_timer[i].time_drifts -= 50;
            }

            if (protocol_timer[i].ticks <= tick_update) {
                tempTimer = (tick_update - protocol_timer[i].ticks);
                tick_update = 1;
                if (tempTimer >= protocol_timer[i].orderedTime) {
                    tick_update += (tempTimer / protocol_timer[i].orderedTime);
                    //time drift
                    protocol_timer[i].time_drifts += ((tempTimer % protocol_timer[i].orderedTime) * 50);
                }

                protocol_timer[i].ticks = 0;
                protocol_timer[i].orderedTime = 0;
                protocol_timer[i].fptr(tick_update);

            } else {
                protocol_timer[i].ticks -= tick_update;
            }
        }
    }

}

void protocol_timer_event_lock_free(void)
{
    platform_enter_critical();
    protocol_tick_handle_busy = false;
    platform_exit_critical();
}


void protocol_timer_cb(uint16_t ticks)
{
    uint8_t i;
    uint16_t tick_update, tempTimer;
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    // The callbacks restart timers: re-armed once, at the end
    pt_in_cb = true;
    protocol_timer_ticks_applied += ticks;
#endif
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        if (protocol_timer[i].ticks) {
            if (protocol_timer[i].ticks <= ticks) {
                tempTimer = (ticks - protocol_timer[i].ticks);

                tick_update = 1;
                if (protocol_timer[i].time_drifts >= 50) {
                    tempTimer++;
                    protocol_timer[i].time_drifts -= 50;
                }

                if (tempTimer >= protocol_timer[i].orderedTime) {
                    tick_update += (tempTimer / protocol_timer[i].orderedTime);
                    protocol_timer[i].time_drifts += ((tempTimer % protocol_timer[i].orderedTime) * 50);
                }

                protocol_timer[i].ticks = 0;
                protocol_timer[i].orderedTime = 0;
                protocol_timer[i].fptr(tick_update);

            } else {
                protocol_timer[i].ticks -= ticks;
            }
        }
    }
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    pt_in_cb = false;
    platform_enter_critical();
    pt_rearm(ns_port_time_us());
    platform_exit_critical();
#endif
}

void protocol_timer_interrupt(int timer_id, uint16_t slots)
{
    (void)timer_id;
    (void)slots;
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
    platform_enter_critical();
    uint32_t now = ns_port_time_us();
    uint32_t n = (now - pt_grid_us) / PT_TICK_US;
    pt_grid_us += n * PT_TICK_US;
    protocol_tick_update += n;
    if (!protocol_tick_update) {
        // Early, or a wake on the way to a later expiry: nothing to count
        pt_rearm(now);
        platform_exit_critical();
        return;
    }
    platform_exit_critical();
#else
    eventOS_callback_timer_start(protocol_timer_id, PROTOCOL_TIMER_INTERVAL);
    protocol_tick_update++;
#endif

    if (!protocol_tick_handle_busy) {
        /* This static stuff gets initialised once */
        static arm_event_storage_t event = {
            .data = {
                .data_ptr = NULL,
                .event_type = ARM_IN_PROTOCOL_TIMER_EVENT,
                .event_id = 0,
                .priority = ARM_LIB_HIGH_PRIORITY_EVENT
            }
        };

        /* Dynamic stuff */
        event.data.receiver = event.data.sender = protocol_read_tasklet_id();
        event.data.event_data = protocol_tick_update;
        protocol_tick_update = 0;

        /* Use user-allocated variant to avoid memory allocation failure */
        eventOS_event_send_user_allocated(&event);
        protocol_tick_handle_busy = true;
    }
}

#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
bool protocol_timer_event_pending(void)
{
    return protocol_tick_handle_busy;
}

/* Called with the lock held. The grid points counted by the callback timer
 * and not yet handed to protocol_timer_cb() (protocol_tick_update) still
 * count against every running timer. */
static void pt_catch_up(uint32_t now)
{
    uint32_t n = (now - pt_grid_us) / PT_TICK_US;
    uint32_t pending = protocol_tick_update;
    uint8_t i;

    if (!n) {
        return;
    }
    // Stop before the earliest expiry: the callback timer counts that one
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        if (protocol_timer[i].ticks) {
            uint32_t left = protocol_timer[i].ticks > pending ? protocol_timer[i].ticks - pending : 0;
            if (left <= 1) {
                return;
            }
            if (n > left - 1) {
                n = left - 1;
            }
        }
    }
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        if (protocol_timer[i].ticks) {
            protocol_timer[i].ticks -= n;
        }
    }
    protocol_timer_ticks_applied += n;
    pt_grid_us += n * PT_TICK_US;
}

/* Called with the lock held. Arms the callback timer for the grid point of
 * the earliest expiry, or stops it if no timer runs. While the protocol event
 * is pending, or protocol_timer_cb() runs, the re-arm is left to the end of
 * protocol_timer_cb(). */
static void pt_rearm(uint32_t now)
{
    uint32_t earliest = UINT32_MAX;
    uint64_t delta_us;
    uint8_t i;

    if (pt_in_cb || protocol_tick_handle_busy) {
        return;
    }
    for (i = 0; i < PROTOCOL_TIMER_MAX; i++) {
        if (protocol_timer[i].ticks && protocol_timer[i].ticks < earliest) {
            earliest = protocol_timer[i].ticks;
        }
    }
    if (earliest == UINT32_MAX) {
        eventOS_callback_timer_stop(protocol_timer_id);
        return;
    }
    // Not busy: every grid point counted is applied, so the expiry is
    // relative to pt_grid_us (grid points since then may be uncounted:
    // catch-up stops before an expiry)
    delta_us = (uint64_t)earliest * PT_TICK_US;
    if (delta_us > (uint32_t)(now - pt_grid_us)) {
        delta_us -= (uint32_t)(now - pt_grid_us);
    } else {
        delta_us = 0;
    }
    if (delta_us > PT_MAX_US) {
        delta_us = PT_MAX_US;
    }
#ifdef NS_PORT_TIMER_USEC
    // At least 1 µs: a zero start disarms on some ports
    eventOS_callback_timer_start_usec(protocol_timer_id, delta_us ? (uint32_t)delta_us : 1);
#else
    // Round up to a slot, at least one
    uint32_t slots = (uint32_t)((delta_us + PT_SLOT_US - 1) / PT_SLOT_US);
    eventOS_callback_timer_start(protocol_timer_id, slots ? slots : 1);
#endif
}

void protocol_timer_catch_up(void)
{
    platform_enter_critical();
    pt_catch_up(ns_port_time_us());
    platform_exit_critical();
}

void protocol_timer_start_at(protocol_timer_id_t id, void (*passed_fptr)(uint16_t), uint32_t applied_at)
{
    platform_enter_critical();
    uint32_t now = ns_port_time_us();
    pt_catch_up(now);
    // Relative to the grid points applied, plus those counted and pending
    uint32_t ticks = applied_at - protocol_timer_ticks_applied;
    if ((int32_t)ticks < 1) {
        ticks = 1;
    }
    if (ticks > UINT16_MAX) {
        ticks = UINT16_MAX;
    }
    protocol_timer[id].ticks = (uint16_t)ticks;
    protocol_timer[id].orderedTime = (uint16_t)ticks;
    protocol_timer[id].fptr = passed_fptr;
    pt_rearm(now);
    platform_exit_critical();
}

bool protocol_timer_due_at(protocol_timer_id_t id, uint32_t *applied_at)
{
    bool running;

    platform_enter_critical();
    running = protocol_timer[id].ticks != 0;
    if (running) {
        *applied_at = protocol_timer_ticks_applied + protocol_timer[id].ticks;
    }
    platform_exit_critical();
    return running;
}
#endif
