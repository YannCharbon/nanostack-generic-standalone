/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests of the simulation port (VIRTUAL_TIME.md §4), against a fake
 * conductor over socketpair().
 *
 * Each test forks. The child is the node: it sets up timers and inputs,
 * attaches and calls ns_port_run(), which never returns. Its callbacks report
 * what they see as frames ("xx" + text) on the conductor socket, which the
 * conductor reads before the next "xi", exactly as it would read radio
 * frames. The parent is the conductor: it drives virtual time and checks the
 * reports, the "xi" deadlines and the child's exit status.
 *
 * The event loop is a stub: it "dispatches" g_events_left events, reporting
 * each one.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "nanostack/mac/fhss_config.h"
#include "ns_port/ns_port.h"
#include "common/os_scheduler.h"
#include "common/os_timer.h"
#include "common/fhss_timer.h"
#include "ns_port_sim.h"

#define NEVER NSIM_NEVER

/* ---- output: the parent writes to fd 1 directly, past the port's stdio */

static int g_failures;
static const char *g_test;

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vdprintf(1, fmt, ap);
    va_end(ap);
}

#define FAIL(...) do {                                              \
        say("FAIL %s:%d (%s): ", __FILE__, __LINE__, g_test);       \
        say(__VA_ARGS__);                                           \
        say("\n");                                                  \
        g_failures++;                                               \
    } while (0)
#define CHECK(cond) do { if (!(cond)) FAIL("%s", #cond); } while (0)
#define CHECK_U64(a, b) do {                                        \
        unsigned long long __a = (a), __b = (b);                    \
        if (__a != __b) FAIL("%s = %llu, expected %llu", #a, __a, __b); \
    } while (0)

// In the child, a failed check ends it with status 3.
#define NODE_CHECK(cond) do {                                       \
        if (!(cond)) {                                              \
            dprintf(2, "node check failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            _exit(3);                                               \
        }                                                           \
    } while (0)

/* ---- node side */

static int g_fd = -1;
static int g_events_left;

static void report(const char *fmt, ...)
{
    char buf[256] = "xx";
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf + 2, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    send(g_fd, buf, 2 + n, 0);
}

bool eventOS_scheduler_dispatch_event(void)
{
    if (g_events_left > 0) {
        g_events_left--;
        report("event %llu", (unsigned long long)ns_port_sim_now_us());
    }
    return g_events_left > 0;
}

void eventOS_scheduler_run_until_idle(void)
{
    while (eventOS_scheduler_dispatch_event())
        ;
}

static void node_attach(int fd, int node_id)
{
    g_fd = fd;
    ns_port_init();
    ns_port_sim_attach(fd, node_id);
}

static void timer_cb(int id, uint16_t slots)
{
    report("timer %d %llu", id, (unsigned long long)ns_port_sim_now_us());
}

static void fhss_cb1(const fhss_api_t *api, uint16_t slots)
{
    report("fhss1 %llu", (unsigned long long)ns_port_sim_now_us());
}

static void fhss_cb2(const fhss_api_t *api, uint16_t slots)
{
    report("fhss2 %llu", (unsigned long long)ns_port_sim_now_us());
}

// The program's callback for the conductor fd: reads one frame.
static void frame_cb(void)
{
    char buf[256];
    ssize_t n = recv(g_fd, buf, sizeof(buf) - 1, 0);

    NODE_CHECK(n >= 2);
    buf[n] = '\0';
    report("frame %s %llu", buf + 2, (unsigned long long)ns_port_sim_now_us());
}

/* ---- conductor side */

#define REPORTS_MAX 64

static int c_fd;
static pid_t c_pid;
static char c_reports[REPORTS_MAX][256];
static int c_nreports;

static void c_send(const void *buf, size_t len)
{
    if (send(c_fd, buf, len, MSG_NOSIGNAL) != len)
        FAIL("send: %m");
}

static void c_welcome(uint64_t seed)
{
    struct nsim_welcome w = { .magic = { 'x', 'w' }, .version = NSIM_PROTO_VERSION, .seed = seed };

    c_send(&w, sizeof(w));
}

// Collect the node's reports up to its "xi"; returns its deadline. Returns 0
// and fails if the node closed the socket instead.
static uint64_t c_idle(void)
{
    char buf[256];
    ssize_t n;

    c_nreports = 0;
    for (;;) {
        n = recv(c_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            FAIL("node closed the socket while the conductor waited for \"xi\"");
            return 0;
        }
        buf[n] = '\0';
        if (!memcmp(buf, "xi", 2)) {
            if (n != sizeof(struct nsim_idle))
                FAIL("\"xi\" of %zd bytes", n);
            return ((struct nsim_idle *)buf)->next_us;
        }
        if (c_nreports < REPORTS_MAX)
            strcpy(c_reports[c_nreports++], buf + 2);
    }
}

static void c_advance(uint64_t t, int nframes, ...)
{
    struct nsim_advance a = { .magic = { 'x', 'a' }, .frames = nframes, .now_us = t };
    char buf[256] = "xx";
    const char *frame;
    va_list ap;

    c_send(&a, sizeof(a));
    va_start(ap, nframes);
    while ((frame = va_arg(ap, const char *))) {
        strcpy(buf + 2, frame);
        c_send(buf, 2 + strlen(frame));
    }
    va_end(ap);
}

// Checks that the reports since the last c_idle() are exactly these.
static void c_expect(int line, ...)
{
    const char *want;
    va_list ap;
    int i = 0;

    va_start(ap, line);
    while ((want = va_arg(ap, const char *))) {
        if (i >= c_nreports)
            FAIL("line %d: missing report \"%s\"", line, want);
        else if (strcmp(c_reports[i], want))
            FAIL("line %d: report %d is \"%s\", expected \"%s\"", line, i, c_reports[i], want);
        i++;
    }
    va_end(ap);
    for (; i < c_nreports; i++)
        FAIL("line %d: unexpected report \"%s\"", line, c_reports[i]);
}
#define EXPECT(...) c_expect(__LINE__, __VA_ARGS__, NULL)
#define EXPECT_NONE() c_expect(__LINE__, NULL)

static int c_exit_status(void)
{
    int status;

    if (waitpid(c_pid, &status, 0) != c_pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void c_quit(void)
{
    struct nsim_quit q = { .magic = { 'x', 'q' } };
    int status;

    c_send(&q, sizeof(q));
    status = c_exit_status();
    if (status != 0)
        FAIL("node exit status %d after \"xq\", expected 0", status);
}

/* ---- runner */

static void run_test(const char *name, void (*node)(int fd), void (*conductor)(void))
{
    int sv[2], before = g_failures;

    g_test = name;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        FAIL("socketpair: %m");
        return;
    }
    c_pid = fork();
    if (c_pid == 0) {
        close(sv[0]);
        node(sv[1]);
        _exit(99); // ns_port_run() never returns
    }
    close(sv[1]);
    c_fd = sv[0];
    conductor();
    close(c_fd);
    say("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

/* ---- tests */

// Arm: a start arms at now + slots × 50 µs; the "xi" carries the deadline.
static void node_arm(int fd)
{
    int id = eventOS_callback_timer_register(timer_cb);

    eventOS_callback_timer_start(id, 20);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_arm(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 1000);
    EXPECT_NONE();
    c_advance(1000, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 0 1000");
    c_quit();
}

// Re-arm replaces the deadline; stop disarms; IDs are the lowest free ones.
static int g_ids[4];

static void rearm_cb(int id, uint16_t slots)
{
    report("timer %d %llu", id, (unsigned long long)ns_port_sim_now_us());
    if (ns_port_sim_now_us() == 2000) {
        // Re-arm from its own callback
        eventOS_callback_timer_start(id, 10);
        eventOS_callback_timer_unregister(g_ids[1]);
        NODE_CHECK(eventOS_callback_timer_register(timer_cb) == g_ids[1]);
    }
}

static void node_rearm(int fd)
{
    g_ids[0] = eventOS_callback_timer_register(rearm_cb);
    g_ids[1] = eventOS_callback_timer_register(timer_cb);
    NODE_CHECK(g_ids[0] == 0 && g_ids[1] == 1);
    eventOS_callback_timer_start(g_ids[0], 20);
    eventOS_callback_timer_start(g_ids[0], 40);   // re-arm: 2000
    eventOS_callback_timer_start(g_ids[1], 10);
    eventOS_callback_timer_stop(g_ids[1]);        // stop
    eventOS_callback_timer_stop(g_ids[1]);        // stop again: harmless
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_rearm(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 2000);
    c_advance(2000, 0, NULL);
    CHECK_U64(c_idle(), 2500);
    EXPECT("timer 0 2000");
    c_advance(2500, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 0 2500");
    c_quit();
}

// Zero length disarms (as timerfd_settime() does) and is counted; the count
// is logged at exit.
static void node_zero(int fd)
{
    int a = eventOS_callback_timer_register(timer_cb);
    int b = eventOS_callback_timer_register(timer_cb);
    int c = eventOS_callback_timer_register(timer_cb);

    eventOS_callback_timer_start(a, 10);
    eventOS_callback_timer_start(a, 0);
    eventOS_callback_timer_start_usec(b, 300);
    eventOS_callback_timer_start_usec(b, 0);
    eventOS_callback_timer_start_sec(c, 2);
    eventOS_callback_timer_start_sec(c, 0);
    ns_port_fhss_timer.fhss_timer_start(100, fhss_cb1, NULL);
    ns_port_fhss_timer.fhss_timer_start(0, fhss_cb1, NULL);
    NODE_CHECK(ns_port_sim_zero_starts() == 4);
    NODE_CHECK(eventOS_callback_timer_expired(a));
    // start_sec and start_usec arm too
    eventOS_callback_timer_start_sec(c, 2);
    eventOS_callback_timer_start_usec(b, 300);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_zero(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 300);
    c_advance(300, 0, NULL);
    CHECK_U64(c_idle(), 2000000);
    EXPECT("timer 1 300");
    c_advance(2000000, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 2 2000000");
    c_quit();
}

// Same-time order: callback timers by deadline then ID, then FHSS timers by
// deadline then first use; each disarmed before its callback.
static void order_cb(int id, uint16_t slots)
{
    NODE_CHECK(eventOS_callback_timer_expired(id));
    report("timer %d %llu", id, (unsigned long long)ns_port_sim_now_us());
}

static void node_order(int fd)
{
    int i;

    for (i = 0; i < 4; i++)
        g_ids[i] = eventOS_callback_timer_register(order_cb);
    eventOS_callback_timer_start(g_ids[2], 2);    // 100
    eventOS_callback_timer_start(g_ids[0], 2);    // 100
    eventOS_callback_timer_start(g_ids[1], 1);    // 50
    eventOS_callback_timer_start(g_ids[3], 3);    // 150
    ns_port_fhss_timer.fhss_timer_start(100, fhss_cb2, NULL);  // first used: ID 0
    ns_port_fhss_timer.fhss_timer_start(100, fhss_cb1, NULL);
    ns_port_fhss_timer.fhss_timer_start(50, fhss_cb2, NULL);   // re-armed: 50
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_order(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 50);
    c_advance(50, 0, NULL);
    CHECK_U64(c_idle(), 100);
    EXPECT("timer 1 50", "fhss2 50");
    // Past several deadlines at once: deadline order, then ID
    c_advance(1000, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 0 1000", "timer 2 1000", "timer 3 1000", "fhss1 1000");
    c_quit();
}

// _expired() and _expired_usec(): 1 when unarmed or due, 0 before.
static void expired_probe_cb(int id, uint16_t slots)
{
    // g_ids[1] and g_ids[2] are due now, g_ids[3] is not yet
    NODE_CHECK(eventOS_callback_timer_expired(g_ids[1]));
    NODE_CHECK(eventOS_callback_timer_expired_usec(g_ids[2]));
    NODE_CHECK(!eventOS_callback_timer_expired(g_ids[3]));
    NODE_CHECK(!eventOS_callback_timer_expired_usec(g_ids[3]));
    report("probe %llu", (unsigned long long)ns_port_sim_now_us());
}

static void node_expired(int fd)
{
    int i;

    g_ids[0] = eventOS_callback_timer_register(expired_probe_cb);
    for (i = 1; i < 4; i++)
        g_ids[i] = eventOS_callback_timer_register(timer_cb);
    NODE_CHECK(eventOS_callback_timer_expired(g_ids[1]));       // unarmed
    NODE_CHECK(eventOS_callback_timer_expired_usec(g_ids[1]));
    eventOS_callback_timer_start_usec(g_ids[1], 100);
    NODE_CHECK(!eventOS_callback_timer_expired(g_ids[1]));      // armed
    NODE_CHECK(!eventOS_callback_timer_expired_usec(g_ids[1]));
    eventOS_callback_timer_stop(g_ids[1]);
    NODE_CHECK(eventOS_callback_timer_expired(g_ids[1]));       // stopped
    eventOS_callback_timer_start_usec(g_ids[0], 100);
    eventOS_callback_timer_start_usec(g_ids[1], 100);
    eventOS_callback_timer_start_usec(g_ids[2], 100);
    eventOS_callback_timer_start_usec(g_ids[3], 101);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_expired(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 100);
    c_advance(100, 0, NULL);
    CHECK_U64(c_idle(), 101);
    EXPECT("probe 100", "timer 1 100", "timer 2 100");
    c_advance(101, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 3 101");
    c_quit();
}

// FHSS: remaining slots = deadline - now (0 if due or unarmed), timestamp =
// low 32 bits of now.
static void fhss_probe_cb(int id, uint16_t slots)
{
    const struct fhss_timer *t = &ns_port_fhss_timer;

    NODE_CHECK(t->fhss_get_timestamp(NULL) == (uint32_t)(400 + NSIM_CLOCK32_OFFSET_US));
    NODE_CHECK(t->fhss_get_remaining_slots(fhss_cb1, NULL) == 600);
    NODE_CHECK(t->fhss_get_remaining_slots(fhss_cb2, NULL) == 0);   // stopped
    t->fhss_timer_stop(fhss_cb1, NULL);
    NODE_CHECK(t->fhss_get_remaining_slots(fhss_cb1, NULL) == 0);
    t->fhss_timer_start(1000, fhss_cb1, NULL);                      // 1400
    report("probe");
}

static void node_fhss(int fd)
{
    const struct fhss_timer *t = &ns_port_fhss_timer;
    int id = eventOS_callback_timer_register(fhss_probe_cb);

    NODE_CHECK(t->fhss_resolution_divider == 1);
    NODE_CHECK(t->fhss_get_remaining_slots(fhss_cb1, NULL) == UINT32_MAX); // unknown
    NODE_CHECK(t->fhss_timer_stop(fhss_cb1, NULL) == -1);
    t->fhss_timer_start(1000, fhss_cb1, NULL);
    t->fhss_timer_start(200, fhss_cb2, NULL);
    t->fhss_timer_stop(fhss_cb2, NULL);
    NODE_CHECK(t->fhss_get_remaining_slots(fhss_cb1, NULL) == 1000);
    eventOS_callback_timer_start_usec(id, 400);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_fhss(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 400);
    c_advance(400, 0, NULL);
    CHECK_U64(c_idle(), 1400);
    EXPECT("probe");
    c_advance(1400, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("fhss1 1400");
    c_quit();
}

// 32-bit wrap of ns_port_time_us() (offset: it wraps at 120 s of virtual
// time) and of the 64-bit time's low half; deadlines stay 64-bit.
#define WRAP_T ((1ULL << 32) + 7)

static void wrap_cb(int id, uint16_t slots)
{
    if (ns_port_sim_now_us() == 120000007) {
        NODE_CHECK(ns_port_time_us() == 7);        // the 32-bit clock wrapped
        eventOS_callback_timer_start_sec(id, 4174);        // 4294.000007 s
        report("wrap32");
        return;
    }
    if (ns_port_sim_now_us() < WRAP_T) {
        eventOS_callback_timer_start_usec(id, WRAP_T - ns_port_sim_now_us());
        report("pre");
        return;
    }
    NODE_CHECK(ns_port_sim_now_us() == WRAP_T);
    NODE_CHECK(ns_port_time_us() == (uint32_t)(WRAP_T + NSIM_CLOCK32_OFFSET_US));
    NODE_CHECK(ns_port_fhss_timer.fhss_get_timestamp(NULL) == ns_port_time_us());
    NODE_CHECK(ns_port_time_wall_s() == NSIM_EPOCH_S + WRAP_T / 1000000);
    eventOS_callback_timer_start(id, 100);
    report("wrap");
}

static void node_wrap(int fd)
{
    int id = eventOS_callback_timer_register(wrap_cb);

    NODE_CHECK(ns_port_time_us() == (uint32_t)NSIM_CLOCK32_OFFSET_US);
    NODE_CHECK(ns_port_time_wall_s() == NSIM_EPOCH_S);
    eventOS_callback_timer_start_usec(id, 120000007);   // just past the 32-bit wrap
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_wrap(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 120000007);
    c_advance(120000007, 0, NULL);
    CHECK_U64(c_idle(), 4294000007ULL);
    EXPECT("wrap32");
    c_advance(4294000007ULL, 0, NULL);
    CHECK_U64(c_idle(), WRAP_T);
    EXPECT("pre");
    c_advance(WRAP_T, 0, NULL);
    CHECK_U64(c_idle(), WRAP_T + 5000);
    EXPECT("wrap");
    c_quit();
}

// Frames: delivered right after "xa", one callback per frame, before the
// event loop and the timers due at the same time.
static void frame_signal_cb(void)
{
    frame_cb();
    g_events_left++;
    eventOS_scheduler_signal();
}

static void node_frames(int fd)
{
    int id = eventOS_callback_timer_register(timer_cb);

    eventOS_callback_timer_start(id, 20);         // 1000
    ns_port_posix_add_fd(fd, false, frame_signal_cb, NULL);
    ns_port_posix_add_scheduler(true);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_frames(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 1000);
    // A frame before the timer's deadline
    c_advance(500, 1, "a", NULL);
    CHECK_U64(c_idle(), 1000);
    EXPECT("frame a 500", "event 500");
    // Two frames at the timer's deadline: frames first, then iterations of
    // one event (one_event) and the due timers
    c_advance(1000, 2, "b", "c", NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("frame b 1000", "frame c 1000", "event 1000", "timer 0 1000", "event 1000");
    c_quit();
}

// The port waits for the announced frames, even if they come late.
static void cond_frames_late(void)
{
    struct nsim_advance a = { .magic = { 'x', 'a' }, .frames = 1, .now_us = 700 };

    c_welcome(1);
    CHECK_U64(c_idle(), 1000);
    c_send(&a, sizeof(a));
    usleep(100000);
    c_send("xxlate", 6);
    CHECK_U64(c_idle(), 1000);
    EXPECT("frame late 700", "event 700");
    // Frames are taken only as announced: an "xa" with no frame, then one
    // whose frame was queued with it (the node stopped meanwhile).
    a.frames = 0;
    a.now_us = 800;
    c_send(&a, sizeof(a));
    CHECK_U64(c_idle(), 1000);
    EXPECT_NONE();
    a.frames = 2;
    a.now_us = 900;
    kill(c_pid, SIGSTOP);
    c_send(&a, sizeof(a));
    c_send("xxq1", 4);
    c_send("xxq2", 4);
    kill(c_pid, SIGCONT);
    CHECK_U64(c_idle(), 1000);
    EXPECT("frame q1 900", "frame q2 900", "event 900", "event 900");
    c_quit();
}

// Idle reports: the node runs until idle before each "xi": its own frames
// first, pending() sources served while they return true, poll hooks once per
// iteration, event loop one event per iteration; earliest of all timers.
static int g_pending;

static bool app_pending(void)
{
    return g_pending > 0;
}

static void app_cb(void)
{
    g_pending--;
    report("app %d", g_pending);
}

static void hook(void)
{
    report("hook");
}

static void node_idle(int fd)
{
    int id = eventOS_callback_timer_register(timer_cb);

    // An application fd is never polled: only pending() counts.
    ns_port_posix_add_fd(0, false, app_cb, app_pending);
    ns_port_posix_add_fd(fd, false, frame_cb, NULL);
    ns_port_posix_add_scheduler(true);
    ns_port_add_poll_hook(hook);
    ns_port_posix_set_poll_timeout(&(struct timespec){ 1, 0 });  // no effect
    eventOS_callback_timer_start(id, 100);                       // 5000
    ns_port_fhss_timer.fhss_timer_start(3000, fhss_cb1, NULL);
    g_pending = 2;
    g_events_left = 2;
    eventOS_scheduler_signal();
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_idle(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 3000);
    EXPECT("app 1", "event 0", "hook", "app 0", "event 0", "hook");
    c_advance(3000, 0, NULL);
    CHECK_U64(c_idle(), 5000);
    EXPECT("fhss1 3000", "hook");
    // A wake with nothing due still runs one iteration (poll hooks)
    c_advance(4000, 0, NULL);
    CHECK_U64(c_idle(), 5000);
    EXPECT("hook");
    c_advance(5000, 0, NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("timer 0 5000", "hook");
    c_quit();
}

// The event loop without ns_port_posix_add_scheduler(): until idle, after the
// other inputs.
static void node_until_idle(int fd)
{
    ns_port_posix_add_fd(fd, false, frame_signal_cb, NULL);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_until_idle(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), NEVER);
    EXPECT_NONE();
    c_advance(10, 2, "a", "b", NULL);
    CHECK_U64(c_idle(), NEVER);
    EXPECT("frame a 10", "frame b 10", "event 10", "event 10");
    c_quit();
}

// Power recording: with NSIM_WELCOME_POWER, an "xc" comes right before each
// "xi", with the timer callbacks of that wake per cause (no symbol table
// here: callback timers are "other", FHSS platform timers "fhss") and the
// events dispatched. Without the flag no "xc" is sent (the other tests).
static void node_power(int fd)
{
    int id = eventOS_callback_timer_register(timer_cb);

    eventOS_callback_timer_start(id, 20);                       // 1000
    ns_port_fhss_timer.fhss_timer_start(1000, fhss_cb1, NULL);  // 1000
    ns_port_posix_add_fd(fd, false, frame_signal_cb, NULL);
    node_attach(fd, 1);
    NODE_CHECK(ns_port_sim_power());
    ns_port_run();
}

// Like c_idle(), and also returns the "xc" that must come just before "xi".
static uint64_t c_idle_power(struct nsim_wake *wake)
{
    char buf[256];
    bool seen = false;
    ssize_t n;

    c_nreports = 0;
    for (;;) {
        n = recv(c_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            FAIL("node closed the socket while the conductor waited for \"xi\"");
            return 0;
        }
        buf[n] = '\0';
        if (!memcmp(buf, "xi", 2)) {
            if (!seen)
                FAIL("\"xi\" without \"xc\" before it");
            return ((struct nsim_idle *)buf)->next_us;
        }
        if (!memcmp(buf, "xc", 2)) {
            if (n != sizeof(*wake))
                FAIL("\"xc\" of %zd bytes", n);
            memcpy(wake, buf, sizeof(*wake));
            seen = true;
            continue;
        }
        if (seen)
            FAIL("a message between \"xc\" and \"xi\"");
        if (c_nreports < REPORTS_MAX)
            strcpy(c_reports[c_nreports++], buf + 2);
    }
}

static void cond_power(void)
{
    struct nsim_welcome w = { .magic = { 'x', 'w' }, .version = NSIM_PROTO_VERSION,
                              .flags = NSIM_WELCOME_POWER, .seed = 1 };
    struct nsim_wake wake;
    int i;

    c_send(&w, sizeof(w));
    CHECK_U64(c_idle_power(&wake), 1000);
    CHECK_U64(wake.events, 0);
    for (i = 0; i < NSIM_CAUSE_COUNT; i++)
        CHECK_U64(wake.timers[i], 0);
    c_advance(1000, 1, "a", NULL);
    CHECK_U64(c_idle_power(&wake), NEVER);
    EXPECT("frame a 1000", "event 1000", "timer 0 1000", "fhss1 1000");
    CHECK_U64(wake.timers[NSIM_CAUSE_OTHER], 1);
    CHECK_U64(wake.timers[NSIM_CAUSE_FHSS], 1);
    CHECK_U64(wake.timers[NSIM_CAUSE_TICK], 0);
    // The stub's dispatch returns false after its last event, so the port
    // counts one event fewer than the stub reports: 0 here.
    CHECK_U64(wake.events, 0);
    c_quit();
}

// Random: splitmix64 seeded with mix64(seed + mix64(node ID + 1)).
static uint64_t ref_mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t ref_splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t g_seed;
static int g_node_id;

static void node_random(int fd)
{
    uint8_t buf[20];

    node_attach(fd, g_node_id);
    NODE_CHECK(ns_port_sim_seed() == g_seed);
    ns_port_random_bytes(buf, 3);
    report("%02x%02x%02x", buf[0], buf[1], buf[2]);
    ns_port_random_bytes(buf, sizeof(buf));
    report("%016llx", *(unsigned long long *)buf);
    ns_port_run();
}

static char g_draws[2][256];

static void cond_random(void)
{
    c_welcome(g_seed);
    CHECK_U64(c_idle(), NEVER);
    CHECK(c_nreports == 2);
    snprintf(g_draws[0], sizeof(g_draws[0]), "%s", c_reports[0]);
    snprintf(g_draws[1], sizeof(g_draws[1]), "%s", c_reports[1]);
    c_quit();
}

static void test_random(void)
{
    char want[2][256], first[2][256];
    uint64_t s, v;

    g_seed = 42;
    g_node_id = 7;
    run_test("random", node_random, cond_random);
    memcpy(first, g_draws, sizeof(first));
    // Against the reference: first draw's low 3 bytes, then the 2nd draw
    s = ref_mix64(42 + ref_mix64(8));
    v = ref_splitmix64(&s);
    snprintf(want[0], sizeof(want[0]), "%02x%02x%02x",
             (unsigned)(v & 0xff), (unsigned)(v >> 8 & 0xff), (unsigned)(v >> 16 & 0xff));
    v = ref_splitmix64(&s);
    snprintf(want[1], sizeof(want[1]), "%016llx", (unsigned long long)v);
    g_test = "random";
    if (strcmp(first[0], want[0]) || strcmp(first[1], want[1]))
        FAIL("draws %s %s, expected %s %s", first[0], first[1], want[0], want[1]);

    run_test("random: same seed, same draws", node_random, cond_random);
    CHECK(!memcmp(first, g_draws, sizeof(first)));
    g_seed = 43;
    run_test("random: other seed, other draws", node_random, cond_random);
    CHECK(strcmp(first[1], g_draws[1]));
    g_seed = 42;
    g_node_id = 8;
    run_test("random: other node, other draws", node_random, cond_random);
    CHECK(strcmp(first[1], g_draws[1]));
}

// Random before attach is fatal.
static void node_random_early(int fd)
{
    uint8_t b;

    close(2);   // keep the expected FATAL message out of the test output
    ns_port_random_bytes(&b, 1);
    _exit(0);
}

static void cond_fatal(void)
{
    int status = c_exit_status();

    if (status != 2)
        FAIL("exit status %d, expected 2 (FATAL)", status);
}

// set_busy() is split mode only: fatal.
static void node_busy(int fd)
{
    close(2);
    ns_port_posix_set_busy(app_pending);
    _exit(0);
}

// Time never goes back: fatal.
static void node_back(int fd)
{
    int id = eventOS_callback_timer_register(timer_cb);

    close(2);
    eventOS_callback_timer_start(id, 20);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_back(void)
{
    c_welcome(1);
    CHECK_U64(c_idle(), 1000);
    c_advance(1000, 0, NULL);
    c_idle();
    c_advance(999, 0, NULL);
    cond_fatal();
}

// Quit: "xq" while idle, while frames are expected, before the welcome.
static void node_quit(int fd)
{
    ns_port_posix_add_fd(fd, false, frame_cb, NULL);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_quit_idle(void)
{
    c_welcome(1);
    c_idle();
    c_quit();
}

static void cond_quit_frames(void)
{
    c_welcome(1);
    c_idle();
    c_advance(10, 2, "a", NULL);    // one frame of two, then "xq"
    c_quit();
}

static void cond_quit_welcome(void)
{
    c_quit();
}

// Logs: whole lines on fds 1 and 2 with the virtual time as prefix; lines
// before attach at time 0; partial last line completed at exit; the
// zero-length start count at exit.
static int g_log_pipe[2];

static void log_cb(int id, uint16_t slots)
{
    printf("at %d", 1);
    printf("500\n");
    fprintf(stderr, "err\n");
    report("logged");
}

static void node_logs(int fd)
{
    int id = eventOS_callback_timer_register(log_cb);

    dup2(g_log_pipe[1], 1);
    dup2(g_log_pipe[1], 2);
    close(g_log_pipe[0]);
    close(g_log_pipe[1]);
    printf("before attach\n");
    eventOS_callback_timer_start_usec(id, 1000000);
    eventOS_callback_timer_start_usec(id, 0);
    eventOS_callback_timer_start_usec(id, 1234567);
    node_attach(fd, 1);
    ns_port_run();
}

static void cond_logs(void)
{
    static const char want[] =
        "1700000000.000000 before attach\n"
        "1700000001.234567 at 1500\n"
        "1700000001.234567 err\n"
        "1700000001.234567 ns_port_sim: 1 zero-length timer starts (1 callback, 0 FHSS)\n";
    char buf[1024];
    size_t len = 0;
    ssize_t n;

    close(g_log_pipe[1]);
    c_welcome(1);
    CHECK_U64(c_idle(), 1234567);
    c_advance(1234567, 0, NULL);
    c_idle();
    EXPECT("logged");
    c_quit();
    while ((n = read(g_log_pipe[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += n;
    buf[len] = '\0';
    close(g_log_pipe[0]);
    if (strcmp(buf, want))
        FAIL("log output:\n%s\nexpected:\n%s", buf, want);
}

static void test_logs(void)
{
    if (pipe(g_log_pipe) < 0) {
        FAIL("pipe: %m");
        return;
    }
    run_test("logs: virtual-time line prefix", node_logs, cond_logs);
}

int main(void)
{
    run_test("arm", node_arm, cond_arm);
    run_test("re-arm, stop, re-register", node_rearm, cond_rearm);
    run_test("zero length disarms and is counted", node_zero, cond_zero);
    run_test("same-time order", node_order, cond_order);
    run_test("expired", node_expired, cond_expired);
    run_test("FHSS remaining slots and timestamp", node_fhss, cond_fhss);
    run_test("32-bit wrap of ns_port_time_us()", node_wrap, cond_wrap);
    run_test("frames before events and timers", node_frames, cond_frames);
    run_test("frames: late, none, queued", node_frames, cond_frames_late);
    run_test("event loop until idle by default", node_until_idle, cond_until_idle);
    run_test("idle reports", node_idle, cond_idle);
    run_test("power: \"xc\" before each \"xi\"", node_power, cond_power);
    test_random();
    run_test("random before attach is fatal", node_random_early, cond_fatal);
    run_test("set_busy is fatal", node_busy, cond_fatal);
    run_test("time going back is fatal", node_back, cond_back);
    run_test("quit while idle", node_quit, cond_quit_idle);
    run_test("quit while frames are expected", node_quit, cond_quit_frames);
    run_test("quit before the welcome", node_quit, cond_quit_welcome);
    test_logs();
    say("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
