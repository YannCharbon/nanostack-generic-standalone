/*
 * Porting API: time (docs/GENERICITY_PLAN.md §3.1.5).
 *
 * ns_port_time_us() is the timebase of the radio driver timestamps
 * (PHY_EXTENSION_GET_TIMESTAMP, PHY_EXTENSION_READ_RX_TIME, CSMA backoff_time)
 * and of fhss_get_timestamp(): all of them are absolute µs on this one clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_TIME_H
#define NS_PORT_TIME_H

#include <stdint.h>

/* Monotonic µs, wraps at 2^32 (about 71 min). ISR-safe. */
uint32_t ns_port_time_us(void);

/* Optional: wall-clock seconds, 0 if unknown. Install it with
 * ns_time_api_system_time_callback_set() if the application wants it. */
uint64_t ns_port_time_wall_s(void);

#endif
