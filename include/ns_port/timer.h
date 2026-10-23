/*
 * Porting API: high-resolution callback timers (docs/GENERICITY_PLAN.md §3.1.2).
 *
 * The functions are Nanostack's own eventOS_callback_timer_* API, declared in
 * nanostack-event-loop/eventOS_callback_timer.h; the port implements them.
 * Contract:
 *  - one slot is 50 µs; resolution must be 50 µs or better;
 *  - register returns a timer ID >= 0, or < 0 on failure; at least 12 timers;
 *  - start is one-shot and relative; starting an armed timer re-arms it;
 *  - stop on an idle or expired timer is harmless;
 *  - the callback runs in stack context (or in the MAC context on ports that
 *    declare NS_PORT_HAS_MAC_CONTEXT), never in a raw ISR; its second
 *    argument is 0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_TIMER_H
#define NS_PORT_TIMER_H

#include "nanostack-event-loop/eventOS_callback_timer.h"

#endif
