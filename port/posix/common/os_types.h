/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 */
#ifndef OS_H
#define OS_H

#include <stdint.h>
#include <stdbool.h>

struct slist;

// Port-private state of the POSIX port. The UART fields that used to live
// here belong to the emulator's Spinel transport (GENERICITY_PLAN.md D7).
struct os_ctxt {
    int event_fd[2];
    struct slist *timers;
    struct slist *fhss_timers;
};

// This global variable is necessary for various API of nanostack. Beside this
// case, please never use it. Defined in os_scheduler.c.
extern struct os_ctxt g_os_ctxt;

#endif
