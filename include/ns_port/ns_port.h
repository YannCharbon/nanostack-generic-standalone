/*
 * Nanostack porting API (docs/GENERICITY_PLAN.md §3).
 *
 * What a port provides so that the stack (nanostack/, nanostack-event-loop/,
 * mbed-client-libservice/, rcp/) runs on a platform. The stack itself includes
 * only the ns_port/ headers it needs, never port-private headers.
 *
 * Heap: the stack calls malloc()/free() directly; the port makes them usable
 * from the stack context (§3.1.7).
 * Storage: through ns_file_system_callbacks_set() (§3.1.9).
 * AES: through arm_hal_aes.h, implemented on mbedTLS by rcp/ccm (§3.1.10).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_NS_PORT_H
#define NS_PORT_NS_PORT_H

#include "ns_port/critical.h"
#include "ns_port/fhss_timer.h"
#include "ns_port/random.h"
#include "ns_port/sched.h"
#include "ns_port/time.h"
#include "ns_port/timer.h"
#include "ns_port/trace.h"

#endif
