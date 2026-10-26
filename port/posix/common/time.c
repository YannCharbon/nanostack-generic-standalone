/* SPDX-License-Identifier: Apache-2.0
 * POSIX port: time (ns_port/time.h).
 */
#include <time.h>
#include "ns_port/time.h"

uint32_t ns_port_time_us(void)
{
    struct timespec tp;

    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec * 1000000 + tp.tv_nsec / 1000;
}

uint64_t ns_port_time_wall_s(void)
{
    struct timespec tp;

    clock_gettime(CLOCK_REALTIME, &tp);
    return tp.tv_sec;
}
