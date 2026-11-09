/* SPDX-License-Identifier: Apache-2.0
 *
 * Backoff of a MAC retry after a missing ACK, with FHSS (GS12; the stack's
 * docs/MAC_RETRY.md).
 *
 * Upstream, a retry waits for the FHSS hold (get_retry_period(): until our
 * next TX slot, past the broadcast dwell) plus MIN_FHSS_CSMA_PERIOD_US plus a
 * random backoff of 0 ... 2^BE - 1 units of 20 symbols: 0 ... 3.7 ms at
 * 150 kb/s (BE 5), shorter than one data frame (7.6 ms for 126 bytes). Two
 * routers that do not hear each other and collided at their parent miss
 * their ACKs at nearly the same time, are held to the same TX slot start
 * (both have the same hop count, so the same TX slots) and send again within
 * less than a frame of each other: CCA cannot help, they collide until the
 * retries run out.
 *
 * With NS_MAC_RETRY_SUN_BACKOFF, the random backoff of a retry is counted in
 * the IEEE 802.15.4 backoff unit of the SUN PHYs, aUnitBackoffPeriod =
 * aTurnaroundTime (1 ms for SUN PHYs) + aCcaTime (8 symbols): about 1.05 ms
 * at 150 kb/s, so 0 ... 15.8 ms after the first retry (BE 4) and
 * 0 ... 32.6 ms after the next ones (BE 5). It comes after the hold, and a
 * wait longer than the MAC's longest one is no longer cut (which dropped the
 * random part).
 * First attempts and an LFN's retries (its FHSS hold is already a random
 * 20-45 ms) keep the upstream backoff.
 *
 * Pure functions, for the unit test (tests/mac_retry).
 */
#ifndef MAC_RETRY_BACKOFF_H_
#define MAC_RETRY_BACKOFF_H_

#include <stdint.h>

/* IEEE 802.15.4 PHY constants for the SUN PHYs */
#define MAC_SUN_TURNAROUND_TIME_US  1000    // aTurnaroundTime
#define MAC_SUN_CCA_TIME_SYMBOLS    8       // aCcaTime

/* The MAC's longest wait to a TX time (mcps_calculate_tx_time()) */
#define MAC_TX_TIME_MAX_US          65000

/* aUnitBackoffPeriod of a SUN PHY, in µs, from the symbol time in ns */
static inline uint32_t mac_sun_unit_backoff_us(uint32_t symbol_time_ns)
{
    return MAC_SUN_TURNAROUND_TIME_US + (MAC_SUN_CCA_TIME_SYMBOLS * symbol_time_ns) / 1000;
}

/*
 * Delay from now to a retry's TX time.
 *   hold_us       the FHSS hold (0: none)
 *   random_units  the CSMA draw, 0 ... 2^BE - 1
 *   unit_us       the backoff unit
 *   min_us        the fixed part (MIN_FHSS_CSMA_PERIOD_US, plus the mode
 *                 switch settling time when used)
 * The random backoff comes after the hold. When the total would pass
 * MAC_TX_TIME_MAX_US, where the MAC cuts every wait, the delay ends min_us
 * before the hold does: the CCA then comes before our TX slot, the FHSS
 * refuses the TX there (or, on the broadcast dwell, returns the frame to the
 * queue), and the MAC draws the backoff again, from a hold of about min_us.
 * A cut wait would instead send at a time set by when the ACK was missed,
 * the same for both senders of a collision to within a frame. A hold that is
 * itself random (an FFN without the broadcast schedule: 20-45 ms) is then
 * used, less min_us.
 */
static inline uint32_t mac_retry_backoff_us(uint32_t hold_us, uint32_t random_units, uint32_t unit_us, uint32_t min_us)
{
    uint32_t backoff_us = min_us + random_units * unit_us;
    if (hold_us > min_us && hold_us + backoff_us > MAC_TX_TIME_MAX_US) {
        return hold_us - min_us;
    }
    return hold_us + backoff_us;
}

#endif /* MAC_RETRY_BACKOFF_H_ */
