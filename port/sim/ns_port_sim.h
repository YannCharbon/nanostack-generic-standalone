/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port (NANOSTACK_PORT=nanostack_port_sim): the porting API on
 * virtual time, driven by a conductor over one SOCK_SEQPACKET socket.
 *
 * The port replaces the POSIX port's clock, timers, FHSS timer, run loop and
 * random source, and shares the rest (log, trace, lists, critical sections).
 * Programs keep calling the ns_port_posix_* functions of common/os_scheduler.h.
 *
 * Conductor protocol. One socket message per SOCK_SEQPACKET message, host
 * byte order, every message starts with a two-letter magic. The node side:
 *
 *   1. The program connects to the conductor and sends its own hello (the
 *      emulator's "xh", outside this header), then calls ns_port_sim_attach().
 *   2. attach() waits for "xw" (welcome): the run's base seed. Virtual time
 *      starts at 0.
 *   3. ns_port_run() runs until idle, sends "xi" (idle) with the deadline of
 *      its earliest armed timer, and blocks.
 *   4. The conductor wakes it with "xa" (advance) to a time t >= now. The
 *      "xa" says how many messages follow it (frames, in the program's own
 *      format). The port waits for each of them in turn (blocking: they may
 *      not have arrived yet) and hands it to the callback the program
 *      registered for this fd with ns_port_posix_add_fd(), which reads it.
 *      Then the timers due at t fire, and the node runs until idle again: 3.
 *   5. Anything the program writes to the socket while it runs (frames) is
 *      read by the conductor before the "xi" that follows it.
 *   6. "xq" (quit): the port exits the program with status 0.
 *
 * Idle means: no event-loop event signalled, no timer due, no pending()
 * callback returning true. Code runs in zero virtual time.
 */
#ifndef NS_PORT_SIM_H
#define NS_PORT_SIM_H

#include <stdint.h>

#define NSIM_PROTO_VERSION 1
#define NSIM_NEVER UINT64_MAX   // "xi": no timer armed

struct nsim_welcome {
    char magic[2];              // "xw"
    uint16_t version;           // NSIM_PROTO_VERSION
    uint32_t reserved;
    uint64_t seed;              // the run's base seed
};

struct nsim_advance {
    char magic[2];              // "xa"
    uint16_t frames;            // messages that follow this one, for the program
    uint16_t reserved[2];
    uint64_t now_us;            // new virtual time, never lower than before
};

struct nsim_idle {
    char magic[2];              // "xi"
    uint16_t reserved[3];
    uint64_t next_us;           // earliest armed timer, or NSIM_NEVER
};

struct nsim_quit {
    char magic[2];              // "xq"
};

// Virtual time 0 is this wall-clock date (ns_port_time_wall_s(), and the log
// timestamps), so that runs are identical whenever they are made.
#define NSIM_EPOCH_S 1700000000ULL

// Called once by the program, after it has connected fd to the conductor and
// sent its hello. Blocks until the welcome. The program must also register
// fd with ns_port_posix_add_fd() for its own messages.
void ns_port_sim_attach(int fd, int node_id);

// The run's base seed, from the welcome. Valid after ns_port_sim_attach().
uint64_t ns_port_sim_seed(void);

// Virtual time in µs since the start of the run (64 bits; the porting API's
// ns_port_time_us() is the low 32 bits of it).
uint64_t ns_port_sim_now_us(void);

// Number of zero-length timer starts so far (callback timers + FHSS timers).
// Such a start disarms the timer, as timerfd_settime() does in the POSIX
// port; the count is also logged at exit.
unsigned long ns_port_sim_zero_starts(void);

#endif
