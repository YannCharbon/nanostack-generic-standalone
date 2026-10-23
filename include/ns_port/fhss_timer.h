/*
 * Porting API: FHSS platform timer (docs/GENERICITY_PLAN.md §3.1.6).
 *
 * Nanostack's fhss_timer_t (nanostack/mac/fhss_config.h). Pass it to
 * ws_management_node_init() when the MAC runs in the same image as the stack.
 * Contract: µs resolution (fhss_resolution_divider = 1); one pending timeout
 * per distinct callback, re-arming replaces it; fhss_get_timestamp() equals
 * ns_port_time_us(); callbacks run in stack context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_FHSS_TIMER_H
#define NS_PORT_FHSS_TIMER_H

struct fhss_timer;

extern const struct fhss_timer ns_port_fhss_timer;

#endif
