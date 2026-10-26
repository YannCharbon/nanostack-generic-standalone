/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 *
 * POSIX port: scheduler signal and run loop (ns_port/sched.h), plus the
 * POSIX-only calls that let an application add its file descriptors to
 * ns_port_run() (GENERICITY_PLAN.md §3.1.3).
 */
#ifndef OS_SCHEDULER_H
#define OS_SCHEDULER_H

#include <stdbool.h>
#include <time.h>

#include "ns_port/sched.h"

// Inputs of ns_port_run() are processed in the order they are added, after
// one pselect() per iteration; callback timers and FHSS timers come last.

// Call cb() when fd is readable (except: has an exceptional condition), or
// whenever pending() returns true. pending may be NULL. While pending()
// returns true, ns_port_run() does not block.
void ns_port_posix_add_fd(int fd, bool except, void (*cb)(void), bool (*pending)(void));

// Place the event loop among the inputs. one_event: dispatch one event per
// iteration (so the other inputs are served in between) instead of running
// until idle. Without this call, the event loop runs until idle after the
// file descriptors.
void ns_port_posix_add_scheduler(bool one_event);

// While busy() returns true, ns_port_run() does not block.
void ns_port_posix_set_busy(bool (*busy)(void));

// What "does not block" means above: pselect() waits at most this long.
// Default 0. wsbrd's original loop waited for the leftover timeout of its
// start-up wait instead, and passes it here.
void ns_port_posix_set_poll_timeout(const struct timespec *ts);

#endif
