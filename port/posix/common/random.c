/* SPDX-License-Identifier: Apache-2.0
 * Porting API: random source (ns_port/random.h), from /dev/urandom.
 * The code comes from the former common/rand.c (Arm randLIB port).
 */
#include <stdio.h>
#include "ns_port/random.h"
#include "common/log.h"

void ns_port_random_bytes(void *buf, size_t n)
{
    static FILE *dev_urandom;
    size_t ret;

    if (!dev_urandom)
        dev_urandom = fopen("/dev/urandom", "r");
    ret = fread(buf, 1, n, dev_urandom);
    FATAL_ON(ret != n, 2, );
}
