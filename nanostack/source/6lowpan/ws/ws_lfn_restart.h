/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS_LFN_RESTART_H_
#define WS_LFN_RESTART_H_

#include <stdbool.h>
#include <stdint.h>
#include "nanostack/ws_management_api.h"

/*
 * The LFN's restarts of a failed uplink, against its parent's asynchronous
 * sweeps (docs/LFN_STACK.md §5, §7) [I].
 *
 * After its retries, the MAC restarts a failed frame after min x 2^n to
 * 2 x min x 2^n ms (n = 0, 1, ...), the lower bound capped at max / 2 and
 * the upper one at max (mcps_set_packet_blacklist(), mac_mcps_sap.c). A
 * parent sends its PAN configurations and PAN advertisements as sweeps,
 * one frame per channel; when both come back to back (trickle timers
 * reset), it does not hear for both sweeps. restart_min_ms and
 * restart_max_ms (250, 2000: at least 2.75 s over the 4 restarts of an
 * operational LFN) were sized on EU plans (62 channels at 50 kbit/s, one
 * PAN configuration sweep: about 1.7 s). On the NA plan of 129 channels at
 * 50 kbit/s (mode 1b) the emulator shows a PAN configuration sweep of
 * 3.57 s, a PAN advertisement sweep of 2.14 s, and both back to back
 * (5.73 s) five times in the first 300 s; an uplink whose restarts all fell
 * inside them was lost (FG25 host twin, MCPS-DATA status 233). The LFN
 * scales both values so that the shortest restart span covers both sweeps
 * of its channel plan.
 *
 * Per channel a sweep takes about 2 ms plus the airtime of the frame and 34
 * bytes (emulator, per channel: 16.68 and 27.88 ms for 57- and 127-byte
 * frames at 50 kbit/s, 8.89 and 12.63 ms at 100 kbit/s; the model gives
 * 16.6, 27.8, 9.3 and 14.9 ms). The estimate takes a 150-byte PAN
 * configuration (LFN IEs, LGTK hashes) and an 80-byte PAN advertisement.
 */
#define WS_LFN_SWEEP_CH_OVERHEAD_US     2000u
#define WS_LFN_SWEEP_PHY_BYTES          34u
#define WS_LFN_SWEEP_BYTES              (150u + WS_LFN_SWEEP_PHY_BYTES + 80u + WS_LFN_SWEEP_PHY_BYTES)

/* Both sweeps (PAN configuration, PAN advertisement) of a parent, in ms */
static inline uint32_t ws_lfn_sweeps_ms(uint16_t channels, uint32_t datarate)
{
    if (!datarate) {
        return 0;
    }
    uint64_t per_ch_us = 2u * WS_LFN_SWEEP_CH_OVERHEAD_US + (uint64_t)WS_LFN_SWEEP_BYTES * 8u * 1000000u / datarate;
    return (uint32_t)((uint64_t)channels * per_ch_us / 1000u);
}

/* The shortest time over 'restarts' restarts (the MAC's lower bounds) */
static inline uint32_t ws_lfn_restart_span_ms(uint16_t min_ms, uint16_t max_ms, uint8_t restarts)
{
    uint32_t span = 0;
    for (uint8_t k = 1; k <= restarts; k++) {
        // mcps_set_packet_blacklist(rf_ptr, buffer, k)
        uint32_t lo = 0;
        for (uint8_t i = 0; i < k; i++) {
            lo = (uint32_t)min_ms << i;
            if (lo > max_ms / 2u) {
                break;
            }
        }
        if (lo > max_ms / 2u) {
            lo = max_ms / 2u;
        }
        span += lo;
    }
    return span;
}

/* The configured min and max (ms), scaled up so that the shortest span of
 * 'restarts' restarts covers both sweeps of a plan of 'channels' channels at
 * 'datarate' bit/s; unchanged if it already does */
static inline void ws_lfn_restart_bounds(uint16_t channels, uint32_t datarate, uint8_t restarts,
                                         uint16_t *min_ms, uint16_t *max_ms)
{
    uint32_t need = ws_lfn_sweeps_ms(channels, datarate);
    uint32_t span = ws_lfn_restart_span_ms(*min_ms, *max_ms, restarts);
    if (!span || span >= need) {
        return;
    }
    uint32_t mn = (uint32_t)((uint64_t)*min_ms * need / span) + 1;
    uint32_t mx = (uint32_t)((uint64_t)*max_ms * need / span) + 1;
    *min_ms = mn > 0xffffu ? 0xffffu : (uint16_t)mn;
    *max_ms = mx > 0xffffu ? 0xffffu : (uint16_t)mx;
}

/* The values the LFN gives the MAC: the defaults (WS_LFN_RESTART_MIN_MS_DEFAULT,
 * WS_LFN_RESTART_MAX_MS_DEFAULT) scaled by ws_lfn_restart_bounds(); values
 * set by the user are kept as they are. True if scaled */
static inline bool ws_lfn_restart_effective(uint16_t channels, uint32_t datarate, uint8_t restarts,
                                            uint16_t *min_ms, uint16_t *max_ms)
{
    if (*min_ms != WS_LFN_RESTART_MIN_MS_DEFAULT || *max_ms != WS_LFN_RESTART_MAX_MS_DEFAULT) {
        return false;
    }
    ws_lfn_restart_bounds(channels, datarate, restarts, min_ms, max_ms);
    return *min_ms != WS_LFN_RESTART_MIN_MS_DEFAULT;
}

#endif /* WS_LFN_RESTART_H_ */
