/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS_LFN_JITTER_H_
#define WS_LFN_JITTER_H_

#include <stdint.h>
#include "ns_rand.h"

/*
 * Random spread of an LFN's LPAS sweeps (GS11, docs/LFN_STACK.md §3) [I].
 *
 * A sweep sends one asynchronous frame per channel, each after the MAC's
 * fixed minimum CSMA period for asynchronous frames. LFNs whose sweeps
 * start within a frame's airtime of each other and that do not hear each
 * other collide at the parent on every channel; with a fixed interval on
 * the 1 s seconds timer they stayed together until their clocks' drift
 * separated them (minutes). So every sweep time is drawn at random, in
 * steps of the event loop's 10 ms tick:
 *  - the first sweep after a discovery start: 0.5-1.5 s (mean 1 s, the
 *    former fixed delay);
 *  - the next ones while no LPA comes: lpas_interval_s +- 25 % after the
 *    previous one (mean lpas_interval_s, as before; the shortest gap stays
 *    longer than a sweep's discovery window with the defaults);
 *  - a sweep the seconds timer starts (parent search, background check):
 *    0-990 ms after that second.
 * Each draw is new, so two LFNs that once sweep together do not again.
 */

/* Resolution of the draws: the event loop's tick (EVENTOS_EVENT_TIMER_HZ) */
#define WS_LFN_JITTER_STEP_MS           10
#define WS_LFN_LPAS_FIRST_MIN_MS        500
#define WS_LFN_LPAS_FIRST_MAX_MS        1500
/* The LPAS interval: lpas_interval_s x (1 +- WS_LFN_LPAS_SPREAD_PCT %) */
#define WS_LFN_LPAS_SPREAD_PCT          25
#define WS_LFN_LPAS_DITHER_MAX_MS       990

/* Uniform in [min_ms, max_ms], in steps of WS_LFN_JITTER_STEP_MS from min_ms */
static inline uint32_t ws_lfn_jitter_ms(uint32_t min_ms, uint32_t max_ms)
{
    uint32_t steps = (max_ms - min_ms) / WS_LFN_JITTER_STEP_MS + 1;
    return min_ms + WS_LFN_JITTER_STEP_MS * (rand_get_32bit() % steps);
}

/* The first LPAS sweep after a discovery start (ms from now) */
static inline uint32_t ws_lfn_lpas_first_ms(void)
{
    return ws_lfn_jitter_ms(WS_LFN_LPAS_FIRST_MIN_MS, WS_LFN_LPAS_FIRST_MAX_MS);
}

/* The next LPAS sweep of a discovery (ms after the previous one) */
static inline uint32_t ws_lfn_lpas_next_ms(uint16_t interval_s)
{
    uint32_t ms = (uint32_t)interval_s * 1000;
    uint32_t spread = ms / 100 * WS_LFN_LPAS_SPREAD_PCT;
    return ws_lfn_jitter_ms(ms - spread, ms + spread);
}

/* A sweep that the seconds timer starts: its delay from that second (ms) */
static inline uint32_t ws_lfn_lpas_dither_ms(void)
{
    return ws_lfn_jitter_ms(0, WS_LFN_LPAS_DITHER_MAX_MS);
}

#endif /* WS_LFN_JITTER_H_ */
