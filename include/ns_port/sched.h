/*
 * Porting API: scheduler signal and run loop (docs/GENERICITY_PLAN.md §3.1.3).
 *
 * Threading model, for every port: the stack is single-threaded and
 * run-to-completion. All stack APIs are called from the stack context, the
 * task or thread that runs ns_port_run() (or eventOS_scheduler_run_until_idle()
 * in an application's own loop).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_SCHED_H
#define NS_PORT_SCHED_H

/* eventOS_scheduler_signal(): "events pending", ISR- and task-safe.
 * Declared by the event loop, implemented by the port. */
#include "nanostack-event-loop/eventOS_scheduler.h"

/* Once, from the stack context, after platform_critical_init() and before
 * eventOS_scheduler_init(): sets up what eventOS_scheduler_signal() and
 * ns_port_run() need. */
void ns_port_init(void);

/* Run the event loop; never returns. Each iteration waits until signalled,
 * until a timer expires or until the port's other inputs are ready, then
 * dispatches pending events, expired callback timers and FHSS timers, and runs
 * the poll hooks. The order within an iteration, and whether the events run
 * until idle or one at a time, are up to the port; the POSIX port lets the
 * application choose (port/posix/common/os_scheduler.h). */
void ns_port_run(void) __attribute__((noreturn));

/* Extra work done in each iteration of ns_port_run(), in stack context. */
void ns_port_add_poll_hook(void (*hook)(void));

#endif
