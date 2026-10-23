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

/* Initialise the event loop and run it; never returns. Each iteration:
 * dispatch expired callback timers, FHSS timers and poll hooks, run
 * eventOS_scheduler_run_until_idle(), then sleep until signalled or until the
 * next timer deadline. */
void ns_port_run(void) __attribute__((noreturn));

/* Extra work done in each iteration of ns_port_run(), in stack context. */
void ns_port_add_poll_hook(void (*hook)(void));

#endif
