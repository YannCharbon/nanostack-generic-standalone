/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2021, Silicon Labs
 * Main authors:
 *     - Jérôme Pouiller <jerome.pouiller@silabs.com>
 *
 * ns_port_run() is the pselect() loop that wsbrd, wsnode and wshwsim each
 * had in their main().
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/select.h>

#include "os_scheduler.h"
#include "os_timer.h"
#include "os_types.h"
#include "fhss_timer.h"
#include "slist.h"
#include "utils.h"
#include "log.h"

struct os_ctxt g_os_ctxt = { };

void ns_port_init(void)
{
    struct os_ctxt *ctxt = &g_os_ctxt;

    pipe(ctxt->event_fd);
    fcntl(ctxt->event_fd[1], F_SETPIPE_SZ, sizeof(uint64_t) * 2);
    fcntl(ctxt->event_fd[1], F_SETFL, O_NONBLOCK);
}

void eventOS_scheduler_signal(void)
{
    struct os_ctxt *ctxt = &g_os_ctxt;
    uint64_t val = 'W';

    write(ctxt->event_fd[1], &val, sizeof(val));
}

static struct os_source *source_add(void)
{
    struct os_ctxt *ctxt = &g_os_ctxt;

    FATAL_ON(ctxt->sources_count >= OS_SOURCES_MAX, 2, "too many ns_port_run() inputs");
    return &ctxt->sources[ctxt->sources_count++];
}

void ns_port_posix_add_fd(int fd, bool except, void (*cb)(void), bool (*pending)(void))
{
    struct os_source *src = source_add();

    src->fd = fd;
    src->except = except;
    src->cb = cb;
    src->pending = pending;
}

void ns_port_posix_add_scheduler(bool one_event)
{
    struct os_source *src = source_add();

    src->fd = -1;
    src->one_event = one_event;
    g_os_ctxt.scheduler_added = true;
}

void ns_port_posix_set_busy(bool (*busy)(void))
{
    g_os_ctxt.busy = busy;
}

void ns_port_posix_set_poll_timeout(const struct timespec *ts)
{
    g_os_ctxt.poll_timeout = *ts;
}

void ns_port_add_poll_hook(void (*hook)(void))
{
    struct os_ctxt *ctxt = &g_os_ctxt;

    FATAL_ON(ctxt->poll_hooks_count >= OS_POLL_HOOKS_MAX, 2, "too many poll hooks");
    ctxt->poll_hooks[ctxt->poll_hooks_count++] = hook;
}

static void dispatch_events(struct os_ctxt *ctxt, const struct os_source *src)
{
    uint64_t val;

    read(ctxt->event_fd[0], &val, sizeof(val));
    WARN_ON(val != 'W');
    if (!src->one_event)
        eventOS_scheduler_run_until_idle();
    else if (eventOS_scheduler_dispatch_event())
        eventOS_scheduler_signal();
}

void ns_port_run(void)
{
    struct os_ctxt *ctxt = &g_os_ctxt;
    struct fhss_timer_entry *fhss_timer;
    struct callback_timer *timer;
    struct slist *it, *next;
    struct os_source *src;
    fd_set rfds, efds;
    bool nonblock;
    int maxfd, ret, i;
    uint64_t val;

    if (!ctxt->scheduler_added)
        ns_port_posix_add_scheduler(false);
    for (;;) {
        maxfd = 0;
        nonblock = ctxt->busy && ctxt->busy();
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        for (i = 0; i < ctxt->sources_count; i++) {
            src = &ctxt->sources[i];
            if (src->fd < 0) {
                FD_SET(ctxt->event_fd[0], &rfds);
                maxfd = max(maxfd, ctxt->event_fd[0]);
                continue;
            }
            FD_SET(src->fd, src->except ? &efds : &rfds);
            maxfd = max(maxfd, src->fd);
            if (src->pending && src->pending())
                nonblock = true;
        }
        SLIST_FOR_EACH_ENTRY(ctxt->timers, timer, node) {
            FD_SET(timer->fd, &rfds);
            maxfd = max(maxfd, timer->fd);
        }
        SLIST_FOR_EACH_ENTRY(ctxt->fhss_timers, fhss_timer, node) {
            FD_SET(fhss_timer->fd, &rfds);
            maxfd = max(maxfd, fhss_timer->fd);
        }
        ret = pselect(maxfd + 1, &rfds, NULL, &efds, nonblock ? &ctxt->poll_timeout : NULL, NULL);
        if (ret < 0)
            FATAL(2, "pselect: %m");
        for (i = 0; i < ctxt->sources_count; i++) {
            src = &ctxt->sources[i];
            if (src->fd < 0) {
                if (FD_ISSET(ctxt->event_fd[0], &rfds))
                    dispatch_events(ctxt, src);
            } else if (FD_ISSET(src->fd, &rfds) || FD_ISSET(src->fd, &efds) ||
                       (src->pending && src->pending())) {
                src->cb();
            }
        }
        // A callback may unregister (and free) its own timer: read the next
        // entry first.
        for (it = ctxt->timers; it; it = next) {
            next = it->next;
            timer = container_of(it, struct callback_timer, node);
            if (FD_ISSET(timer->fd, &rfds)) {
                ret = read(timer->fd, &val, sizeof(val));
                WARN_ON(ret < sizeof(val) || val != 1, "cancelled timer?");
                timer->fn(timer->id, 0);
            }
        }
        SLIST_FOR_EACH_ENTRY(ctxt->fhss_timers, fhss_timer, node) {
            if (FD_ISSET(fhss_timer->fd, &rfds)) {
                ret = read(fhss_timer->fd, &val, sizeof(val));
                WARN_ON(ret < sizeof(val) || val != 1, "cancelled timer?");
                fhss_timer->fn(fhss_timer->arg, 0);
            }
        }
        for (i = 0; i < ctxt->poll_hooks_count; i++)
            ctxt->poll_hooks[i]();
    }
}
