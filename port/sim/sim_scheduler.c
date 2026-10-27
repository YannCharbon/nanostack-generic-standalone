/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: scheduler signal and run loop (ns_port/sched.h), the
 * ns_port_posix_* calls of common/os_scheduler.h, and the conductor link
 * (ns_port_sim.h, VIRTUAL_TIME.md §3-§4).
 *
 * ns_port_run() runs the node until it is idle, tells the conductor when its
 * next timer is due ("xi"), and blocks until the conductor moves virtual time
 * to the node's next event ("xa"). Code runs in zero virtual time.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "common/os_scheduler.h"
#include "common/os_types.h"
#include "common/log.h"
#include "ns_port_sim.h"
#include "sim_internal.h"

// A node that does not become idle after this many iterations of one wake is
// stuck in a same-time loop.
#define SIM_ITERATIONS_MAX 10000000

static struct {
    int fd;                     // conductor, -1 before attach
    int node_id;
    uint64_t seed;
    uint64_t now_us;
    volatile bool signalled;

    struct os_source sources[OS_SOURCES_MAX];
    int sources_count;
    bool scheduler_added;
    void (*poll_hooks[OS_POLL_HOOKS_MAX])(void);
    int poll_hooks_count;
} sim = {
    .fd = -1,
};

void ns_port_init(void)
{
    sim_stdio_install();
}

void eventOS_scheduler_signal(void)
{
    sim.signalled = true;
}

static struct os_source *source_add(void)
{
    FATAL_ON(sim.sources_count >= OS_SOURCES_MAX, 2, "too many ns_port_run() inputs");
    return &sim.sources[sim.sources_count++];
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
    sim.scheduler_added = true;
}

void ns_port_posix_set_busy(bool (*busy)(void))
{
    FATAL(2, "ns_port_posix_set_busy(): not supported by the simulation port");
}

// Nothing is polled: there is no timeout to set.
void ns_port_posix_set_poll_timeout(const struct timespec *ts)
{
}

void ns_port_add_poll_hook(void (*hook)(void))
{
    FATAL_ON(sim.poll_hooks_count >= OS_POLL_HOOKS_MAX, 2, "too many poll hooks");
    sim.poll_hooks[sim.poll_hooks_count++] = hook;
}

uint64_t ns_port_sim_now_us(void)
{
    return sim.now_us;
}

uint64_t ns_port_sim_seed(void)
{
    return sim.seed;
}

unsigned long ns_port_sim_zero_starts(void)
{
    return g_sim_zero_starts_timer + g_sim_zero_starts_fhss;
}

static void sim_report(void)
{
    INFO("ns_port_sim: %lu zero-length timer starts (%lu callback, %lu FHSS)",
         ns_port_sim_zero_starts(), g_sim_zero_starts_timer, g_sim_zero_starts_fhss);
}

static bool magic_is(const void *buf, const char *magic)
{
    return !memcmp(buf, magic, 2);
}

static void sim_quit(void)
{
    exit(0);
}

// One blocking receive on the conductor socket, retried on EINTR.
static ssize_t sim_recv(void *buf, size_t len, int flags)
{
    ssize_t ret;

    for (;;) {
        ret = recv(sim.fd, buf, len, flags);
        if (ret < 0 && errno == EINTR)
            continue;
        FATAL_ON(ret < 0, 2, "conductor: recv: %m");
        FATAL_ON(ret == 0, 2, "conductor closed the connection");
        return ret;
    }
}

static void sim_send(const void *buf, size_t len)
{
    ssize_t ret;

    do
        ret = send(sim.fd, buf, len, MSG_NOSIGNAL);
    while (ret < 0 && errno == EINTR);
    FATAL_ON(ret != len, 2, "conductor: send: %m");
}

void ns_port_sim_attach(int fd, int node_id)
{
    union {
        struct nsim_welcome welcome;
        char raw[64];
    } msg;
    ssize_t ret;

    FATAL_ON(sim.fd >= 0, 2, "ns_port_sim_attach() called twice");
    sim_stdio_install();
    sim.fd = fd;
    sim.node_id = node_id;
    ret = sim_recv(&msg, sizeof(msg), 0);
    if (ret >= 2 && magic_is(&msg, "xq"))
        sim_quit();
    FATAL_ON(ret < sizeof(msg.welcome) || !magic_is(&msg, "xw"), 2,
             "conductor: expected a welcome, got %.2s (%zd bytes)", msg.raw, ret);
    FATAL_ON(msg.welcome.version != NSIM_PROTO_VERSION, 2,
             "conductor: protocol version %u, expected %u",
             msg.welcome.version, NSIM_PROTO_VERSION);
    sim.seed = msg.welcome.seed;
    sim_random_seed(sim.seed, node_id);
    atexit(sim_report);
}

static struct os_source *conductor_source(void)
{
    int i;

    for (i = 0; i < sim.sources_count; i++)
        if (sim.sources[i].fd == sim.fd)
            return &sim.sources[i];
    return NULL;
}

// Hand each of the frames announced by "xa" to the program's callback for
// the conductor fd, which reads it. They may not have arrived yet: wait for
// each (a blocking peek).
static void sim_deliver_frames(unsigned int frames)
{
    struct os_source *src = conductor_source();
    char magic[2];
    unsigned int i;
    ssize_t ret;

    for (i = 0; i < frames; i++) {
        ret = sim_recv(magic, sizeof(magic), MSG_PEEK);
        FATAL_ON(ret < sizeof(magic), 2, "conductor: short message (%zd bytes)", ret);
        if (magic_is(magic, "xq"))
            sim_quit();
        FATAL_ON(magic_is(magic, "xa") || magic_is(magic, "xw"), 2,
                 "conductor: %.2s where a frame was announced", magic);
        FATAL_ON(!src, 2, "conductor: frame for a node that registered no callback for fd %d", sim.fd);
        src->cb();
    }
}

// Block until the conductor moves time forward, then take the frames.
static void sim_wait_advance(void)
{
    union {
        struct nsim_advance advance;
        char raw[64];
    } msg;
    ssize_t ret;

    ret = sim_recv(&msg, sizeof(msg), 0);
    if (ret >= 2 && magic_is(&msg, "xq"))
        sim_quit();
    FATAL_ON(ret < sizeof(msg.advance) || !magic_is(&msg, "xa"), 2,
             "conductor: expected an advance, got %.2s (%zd bytes)", msg.raw, ret);
    FATAL_ON(msg.advance.now_us < sim.now_us, 2,
             "conductor: time goes back from %llu to %llu µs",
             (unsigned long long)sim.now_us, (unsigned long long)msg.advance.now_us);
    sim.now_us = msg.advance.now_us;
    sim_deliver_frames(msg.advance.frames);
}

static void sim_send_idle(void)
{
    struct nsim_idle idle = { .magic = { 'x', 'i' } };
    uint64_t t, f;

    t = sim_timer_next();
    f = sim_fhss_next();
    idle.next_us = t < f ? t : f;
    sim_send(&idle, sizeof(idle));
}

static bool sim_sources_pending(void)
{
    const struct os_source *src;
    int i;

    for (i = 0; i < sim.sources_count; i++) {
        src = &sim.sources[i];
        if (src->fd >= 0 && src->fd != sim.fd && src->pending && src->pending())
            return true;
    }
    return false;
}

static bool sim_idle(void)
{
    return !sim.signalled && !sim_sources_pending() &&
           !sim_timer_due(sim.now_us) && !sim_fhss_due(sim.now_us);
}

// One iteration: the sources in the order they were added, then the due
// callback timers, the due FHSS timers and the poll hooks. Application file
// descriptors are never polled: only their pending() counts. The conductor's
// fd is served by sim_deliver_frames().
static void sim_iterate(void)
{
    const struct os_source *src;
    int i;

    for (i = 0; i < sim.sources_count; i++) {
        src = &sim.sources[i];
        if (src->fd < 0) {
            if (!sim.signalled)
                continue;
            sim.signalled = false;
            if (!src->one_event)
                eventOS_scheduler_run_until_idle();
            else if (eventOS_scheduler_dispatch_event())
                eventOS_scheduler_signal();
        } else if (src->fd != sim.fd && src->pending && src->pending()) {
            src->cb();
        }
    }
    sim_timer_fire_due(sim.now_us);
    sim_fhss_fire_due(sim.now_us);
    for (i = 0; i < sim.poll_hooks_count; i++)
        sim.poll_hooks[i]();
}

void ns_port_run(void)
{
    long n;

    FATAL_ON(sim.fd < 0, 2, "ns_port_run(): ns_port_sim_attach() was not called");
    if (!sim.scheduler_added)
        ns_port_posix_add_scheduler(false);
    for (;;) {
        n = 0;
        do {
            FATAL_ON(++n > SIM_ITERATIONS_MAX, 2,
                     "node %d not idle after %ld iterations at %llu µs",
                     sim.node_id, n - 1, (unsigned long long)sim.now_us);
            sim_iterate();
        } while (!sim_idle());
        sim_send_idle();
        sim_wait_advance();
    }
}
