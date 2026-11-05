/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: time (ns_port/time.h) on virtual time.
 */
#include "ns_port/time.h"
#include "ns_port_sim.h"

uint32_t ns_port_time_us(void)
{
    return (uint32_t)(ns_port_sim_local_us() + NSIM_CLOCK32_OFFSET_US);
}

// The date: the run's, from the node's clock (a node that starts late, as a
// device set to the date at power-up, keeps the same date as the others).
uint64_t ns_port_time_wall_s(void)
{
    return NSIM_EPOCH_S + (ns_port_sim_start_us() + ns_port_sim_local_us()) / 1000000;
}
