/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: what its files share. Not for programs.
 */
#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

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

#endif
