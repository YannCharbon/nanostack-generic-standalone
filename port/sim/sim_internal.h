/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: what its files share. Not for programs.
 */
#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

// sim_scheduler.c: the node's clock (drift). sim_local_of() maps virtual
// time to the node's clock; sim_deadline_after() is the virtual time at which
// the node's clock has advanced by us from now.
uint64_t sim_local_of(uint64_t virtual_us);
uint64_t sim_deadline_after(uint64_t us);

// sim_timer.c: callback timers
bool sim_timer_due(uint64_t now);
void sim_timer_fire_due(uint64_t now);
uint64_t sim_timer_next(void);

// sim_fhss_timer.c: FHSS timers
bool sim_fhss_due(uint64_t now);
void sim_fhss_fire_due(uint64_t now);
uint64_t sim_fhss_next(void);

// Zero-length starts, which disarm (callback timers, FHSS timers)
extern unsigned long g_sim_zero_starts_timer;
extern unsigned long g_sim_zero_starts_fhss;

// sim_random.c
void sim_random_seed(uint64_t seed, int node_id);

// sim_stdio.c
void sim_stdio_install(void);

// sim_power.c: the "xc" report of each wake, when the conductor records
// power (g_sim_power, from the welcome)
struct nsim_wake;
extern bool g_sim_power;
void sim_power_timer_fired(const void *fn, bool fhss);
void sim_power_events(unsigned int n);
void sim_power_take(struct nsim_wake *out);

#endif
