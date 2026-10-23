/*
 * Porting API: random source (docs/GENERICITY_PLAN.md §3.1.4).
 *
 * The stack's rand_* helpers (ns_rand.h) are built on this function, and the
 * TLS DRBG is seeded from them (D13), so on hardware it must be a real TRNG.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_RANDOM_H
#define NS_PORT_RANDOM_H

#include <stddef.h>

/* Fill buf with n bytes of cryptographically strong random data. Must not
 * block for more than about 1 ms. Stack context; ISR-safe only if the port
 * says so. */
void ns_port_random_bytes(void *buf, size_t n);

#endif
