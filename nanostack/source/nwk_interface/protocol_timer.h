/*
 * Copyright (c) 2014-2017, Pelion and affiliates.
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

#ifndef PROTOCOL_TIMER_H_
#define PROTOCOL_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROTOCOL_TIMER_STACK_TIM,
    PROTOCOL_TIMER_TCP_TIM,
    PROTOCOL_TIMER_MULTICAST_TIM,
    PROTOCOL_TIMER_MAX
} protocol_timer_id_t;

typedef struct {
    void (*fptr)(uint16_t);
    uint16_t ticks;
    uint16_t orderedTime;
    uint16_t time_drifts;
} protocol_timer_t;

extern int protocol_timer_init(void);
extern void protocol_timer_event_lock_free(void);
extern void protocol_timer_cb(uint16_t ticks);
extern void protocol_timer_interrupt(int timer_id, uint16_t slots);
extern void protocol_timer_start(protocol_timer_id_t id, void (*passed_fptr)(uint16_t), uint32_t time_ms);
extern void protocol_timer_stop(protocol_timer_id_t id);
extern void protocol_timer_sleep_balance(uint32_t time_in_ms);

#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
/* Idle-tick skipping of the protocol timers (docs/PORTING.md, "Idle-tick
 * skipping of the protocol timers"). The 50 ms grid stays; the callback timer
 * is armed only for the grid point at which the earliest timer expires. */

/* "Nothing due" in the timer_next() functions of the modules */
#define PROTOCOL_TIMER_NEXT_NONE    UINT32_MAX

static inline uint32_t protocol_timer_next_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/* 50 ms grid points applied to the timers since protocol_timer_init() */
extern uint32_t protocol_timer_ticks_applied;

/* Applies the grid points passed since the last count to the running timers,
 * up to the one before the earliest expiry (that one is the callback
 * timer's). No callback is called. */
extern void protocol_timer_catch_up(void);

/* Starts a timer for the grid point at which protocol_timer_ticks_applied
 * reaches 'applied_at' (at least one grid point from now). */
extern void protocol_timer_start_at(protocol_timer_id_t id, void (*passed_fptr)(uint16_t), uint32_t applied_at);

/* Grid point (in protocol_timer_ticks_applied) at which a timer expires, or
 * false if it is not running. */
extern bool protocol_timer_due_at(protocol_timer_id_t id, uint32_t *applied_at);

/* The protocol event is posted and not yet handled */
extern bool protocol_timer_event_pending(void);
#endif

#endif /* PROTOCOL_TIMER_H_ */
