/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 */
#ifndef OS_H
#define OS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

struct slist;

// One input of ns_port_run(): an application file descriptor, or the event
// loop itself (fd < 0), in the order they are processed (os_scheduler.h).
struct os_source {
    int fd;
    bool except;
    bool one_event;
    bool (*pending)(void);
    void (*cb)(void);
};

#define OS_SOURCES_MAX 8
#define OS_POLL_HOOKS_MAX 4

// Port-private state of the POSIX port. The UART fields that used to live
// here belong to the emulator's Spinel transport (GENERICITY_PLAN.md D7).
struct os_ctxt {
    int event_fd[2];
    struct slist *timers;
    struct slist *fhss_timers;

    struct os_source sources[OS_SOURCES_MAX];
    int sources_count;
    bool scheduler_added;
    bool (*busy)(void);
    struct timespec poll_timeout;
    void (*poll_hooks[OS_POLL_HOOKS_MAX])(void);
    int poll_hooks_count;
};

// This global variable is necessary for various API of nanostack. Beside this
// case, please never use it. Defined in os_scheduler.c.
extern struct os_ctxt g_os_ctxt;

#endif
