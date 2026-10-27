/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: time (ns_port/time.h) on virtual time.
 */
#include "ns_port/time.h"
#include "ns_port_sim.h"

uint32_t ns_port_time_us(void)
{
    return (uint32_t)ns_port_sim_now_us();
}

uint64_t ns_port_time_wall_s(void)
{
    return NSIM_EPOCH_S + ns_port_sim_now_us() / 1000000;
}
