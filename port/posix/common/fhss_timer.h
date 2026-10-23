/* SPDX-License-Identifier: Apache-2.0
 * POSIX port: FHSS platform timer (ns_port/fhss_timer.h), one timerfd per
 * callback. The run loop polls the timers listed in g_os_ctxt.fhss_timers.
 */
#ifndef FHSS_TIMER_H
#define FHSS_TIMER_H

#include <stdint.h>

#include "nanostack/mac/fhss_config.h"
#include "ns_port/fhss_timer.h"
#include "common/slist.h"

struct fhss_timer_entry {
    int fd;
    const fhss_api_t *arg;
    void (*fn)(const fhss_api_t *api, uint16_t);
    struct slist node;
};

#endif
