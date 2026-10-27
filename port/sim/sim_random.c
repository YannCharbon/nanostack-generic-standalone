/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: random source (ns_port/random.h). A splitmix64 stream
 * seeded from the run's seed and the node ID (VIRTUAL_TIME.md §3), so a run
 * is reproducible. Not a TRNG: never use this port outside the emulator.
 */
#include <stdbool.h>
#include <string.h>

#include "ns_port/random.h"
#include "common/log.h"
#include "sim_internal.h"

#define GOLDEN_GAMMA 0x9E3779B97F4A7C15ULL

static uint64_t state;
static bool seeded;

// splitmix64's output function, used to spread the seeds.
static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Each node's start state is hashed from the run's seed and the node ID. A
// plain combination such as seed ^ (GOLDEN_GAMMA * (id + 1)) would put nodes
// on the same sequence, shifted by a few draws, since the state steps by
// GOLDEN_GAMMA.
void sim_random_seed(uint64_t seed, int node_id)
{
    state = mix64(seed + mix64((uint64_t)node_id + 1));
    seeded = true;
}

static uint64_t splitmix64(void)
{
    uint64_t z = (state += GOLDEN_GAMMA);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// One 64-bit draw per 8 bytes; the leftover bytes of the last draw are lost.
void ns_port_random_bytes(void *buf, size_t n)
{
    uint8_t *p = buf;
    uint64_t val;
    size_t len;

    FATAL_ON(!seeded, 2, "ns_port_random_bytes() before ns_port_sim_attach()");
    while (n) {
        val = splitmix64();
        len = n < sizeof(val) ? n : sizeof(val);
        memcpy(p, &val, len);
        p += len;
        n -= len;
    }
}
