/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the backoff of a MAC retry after a missing ACK, with FHSS
 * (GS12; mac_pd_sap.c, mac_csma_backoff_get(); mac_retry_backoff.h;
 * docs/MAC_RETRY.md), with a fake FHSS hold (get_retry_period) and scripted
 * random draws, at the emulator's PHY (150 kb/s: 6666 ns symbols).
 *
 * The case it guards (emulator, central medium): two routers that do not
 * hear each other collide at the border router, miss their ACKs within a
 * frame of each other, are held to the same TX slot start, and send again
 * after 5 ms plus 0 ... 2^BE - 1 units of 20 symbols (at most 3.7 ms),
 * less than one data frame (7.6 ms for 126 bytes): they collide again
 * whatever the draws, until the retries run out.
 *
 * Checks, for both variants: a first attempt and an LFN's retry keep the
 * upstream backoff (the FHSS hold only for a retry); the draw covers
 * 0 ... 2^BE - 1. Then, over every pair of draws, the share of retries of
 * two such routers that start within one frame of each other (a collision
 * again): upstream (built without NS_MAC_RETRY_SUN_BACKOFF) every pair, at
 * BE 4 and 5, with or without a hold; with the fix, at most 72 % at BE 4 and
 * 42 % at BE 5 (units of 1 ms + 8 symbols), and 13 % for all three retries.
 * With the fix also: the backoff comes after the hold, and a total past the
 * MAC's longest wait (65 ms) ends 5 ms before the hold does.
 *
 *   tests/mac_retry/run.sh [build dir]
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nsconfig.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/fhss_api.h"
#include "mac_defines.h"
#include "mac_data_buffer.h"
#include "mac_pd_sap.h"
#include "mac_retry_backoff.h"

static unsigned checks, failures;
#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#ifdef NS_MAC_RETRY_SUN_BACKOFF
#define VARIANT "fix"
#else
#define VARIANT "upstream"
#endif

#define SYMBOL_NS       6666    // the emulator's PHY, 150 kb/s
#define AIRTIME_US      7573    // a 126-byte frame at that rate (emulator logs)
#define OLD_UNIT_US     (20 * (SYMBOL_NS / 1000))

/* ------------------------------------------------- fakes: random, FHSS */

static uint16_t draw;           // the next value of rand_get_random_in_range()
static uint16_t draw_max;       // the max of its last call
static unsigned draws;

uint16_t rand_get_random_in_range(uint16_t min, uint16_t max)
{
    draws++;
    draw_max = max;
    return draw < min ? min : draw > max ? max : draw;
}

static uint32_t hold_us;        // what get_retry_period returns
static unsigned holds;

static uint32_t fake_get_retry_period(const fhss_api_t *api, uint8_t *dst, uint16_t mtu)
{
    (void)api; (void)dst; (void)mtu;
    holds++;
    return hold_us;
}

static fhss_api_t fhss;
static mac_pre_build_frame_t frame;
static protocol_interface_rf_mac_setup_s mac;

static void setup(uint8_t tx_retry, bool lfn)
{
    memset(&fhss, 0, sizeof(fhss));
    memset(&frame, 0, sizeof(frame));
    memset(&mac, 0, sizeof(mac));
    fhss.get_retry_period = fake_get_retry_period;
    mac.fhss_api = &fhss;
    mac.active_pd_data_request = &frame;
    mac.rf_csma_extension_supported = true;
    mac.aUnitBackoffPeriod = 20;
    mac.symbol_time_ns = SYMBOL_NS;
    mac.multi_cca_interval = 2000;
    mac.number_of_csma_ca_periods = 2;
    mac.macMinBE = 3;
    mac.macMaxBE = 5;
    mac.mac_tx_retry = tx_retry;
    mac.mac_rx_windows = lfn;
    mac_csma_param_init(&mac);
    // the MAC raises BE once per retry (mac_sap_no_ack_cb())
    for (int i = 0; i < tx_retry && mac.macCurrentBE < mac.macMaxBE; i++) {
        mac.macCurrentBE++;
    }
    draws = holds = 0;
}

static uint32_t backoff(uint16_t r, uint32_t hold)
{
    draw = r;
    hold_us = hold;
    return mac_csma_backoff_get(&mac);
}

/* The upstream backoff: 20-symbol units, 5 ms added below 5 ms, the hold */
static uint32_t upstream(uint16_t r, uint32_t hold)
{
    uint32_t b = r * OLD_UNIT_US;
    if (!b) {
        b = 1;
    }
    if (b < 5000) {
        b += 5000;
    }
    return b + hold;
}

/* ------------------------------------------------------------- tests */

static void test_first_attempt(void)
{
    setup(0, false);
    CHECK(backoff(7, 30000) == upstream(7, 0));
    CHECK(holds == 0 && draws == 1 && draw_max == 7);           // BE 3
}

static void test_lfn_retry(void)
{
    setup(1, true);
    CHECK(backoff(9, 30000) == upstream(9, 30000));
    CHECK(holds == 1 && draw_max == 15);                         // BE 4
}

static void test_retry_backoff(void)
{
    setup(1, false);
    uint32_t unit = mac_sun_unit_backoff_us(SYMBOL_NS);
    CHECK(unit == 1053);
    CHECK(mac_sun_unit_backoff_us(20000) == 1160);               // 50 kb/s
    CHECK(backoff(0, 0) && draw_max == 15);
    setup(2, false);
    CHECK(backoff(0, 0) && draw_max == 31);
    setup(3, false);
    CHECK(backoff(0, 0) && draw_max == 31);                      // macMaxBE
#ifdef NS_MAC_RETRY_SUN_BACKOFF
    setup(1, false);
    CHECK(backoff(0, 0) == 5000);
    CHECK(backoff(7, 0) == 5000 + 7 * unit);
    CHECK(backoff(15, 20000) == 20000 + 5000 + 15 * unit);       // after the hold
    CHECK(holds == 3);
    // past 65 ms: 5 ms before the hold ends (the FHSS refuses the CCA there)
    CHECK(backoff(10, 60000) == 55000);
    CHECK(backoff(0, 80000) == 75000);                           // the MAC cuts it to 65 ms
    CHECK(backoff(0, 60000) == 65000);                           // fits
    frame.phy_mode_id = 1;                                       // mode switch settling time
    CHECK(backoff(2, 0) == 5000 + 510 + 2 * unit);
#else
    setup(1, false);
    CHECK(backoff(7, 20000) == upstream(7, 20000));
#endif
}

/*
 * Two routers hidden from each other miss their ACKs at t0 and t0 + skew
 * (skew < one frame: their frames overlapped), with the same hold to a
 * common slot start (20 ms: the total stays under 65 ms), or none. Share of
 * draw pairs whose TX times are less than one frame apart.
 */
static double lockstep_share(uint8_t tx_retry, uint32_t to_slot_us, uint32_t skew_us)
{
    unsigned pairs = 0, again = 0;
    setup(tx_retry, false);
    unsigned n = 1u << mac.macCurrentBE;
    for (unsigned a = 0; a < n; a++) {
        for (unsigned b = 0; b < n; b++) {
            int64_t ta = backoff(a, to_slot_us);
            int64_t tb = skew_us + backoff(b, to_slot_us ? to_slot_us - skew_us : 0);
            int64_t d = ta > tb ? ta - tb : tb - ta;
            pairs++;
            if (d < AIRTIME_US) {
                again++;
            }
        }
    }
    return (double)again / pairs;
}

static void test_lockstep(void)
{
    double be4_hold = lockstep_share(1, 20000, 3000);
    double be5_hold = lockstep_share(2, 20000, 3000);
    double be4_none = lockstep_share(1, 0, 0);
    double be5_none = lockstep_share(2, 0, 0);
    double chain = be4_hold * be5_hold * be5_hold;
    printf("mac_retry %s: retries less than one frame apart: BE 4 %.1f %%, BE 5 %.1f %% (held to the same slot; "
           "%.1f %%, %.1f %% without a hold); all three retries %.1f %%\n", VARIANT,
           100 * be4_hold, 100 * be5_hold, 100 * be4_none, 100 * be5_none, 100 * chain);
#ifdef NS_MAC_RETRY_SUN_BACKOFF
    CHECK(be4_hold <= 0.72 && be4_none <= 0.72);
    CHECK(be5_hold <= 0.42 && be5_none <= 0.42);
    CHECK(chain <= 0.13);
#else
    // the defect: every pair collides again
    CHECK(be4_hold == 1.0 && be5_hold == 1.0 && be4_none == 1.0 && be5_none == 1.0);
#endif
}

int main(void)
{
    test_first_attempt();
    test_lfn_retry();
    test_retry_backoff();
    test_lockstep();
    printf("mac_retry %s: %u checks, %u failed\n", VARIANT, checks, failures);
    return failures ? 1 : 0;
}
