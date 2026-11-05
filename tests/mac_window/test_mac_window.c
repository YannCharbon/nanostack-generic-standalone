/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of an LFN's timed receive windows in the MAC (mac_mlme.c,
 * mac_mlme_rx_window(); docs/LFN_MAC.md §3) and of the rule that turns its
 * radio off after a data confirm (mac_lfn_async.c,
 * mac_data_poll_radio_disable_check() and mac_lfn_radio_stays_on(), called
 * by mac_mcps_sap.c at its data confirms), on a fake
 * PHY that records its state changes.
 *
 * The case it guards (emulator test_lfn_left.sh, seed 1): a window closes
 * while the radio receives a frame, so the MAC defers the close (returns 1)
 * and the FHSS asks again shortly. Meanwhile a data confirm (a queued
 * uplink's timeout, read from the queue at its restart) asked whether the
 * radio may go off: with the window marked closed it said yes, and the
 * radio was turned off in the middle of the frame. Now the deferred close
 * keeps it on, and the FHSS's next close turns it off once the frame ends.
 * Unchanged: an open window keeps the radio on; outside any window (an ACK
 * wait, for instance) a confirm turns it off, as before.
 *
 *   tests/mac_window/run.sh [build dir]
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nsconfig.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/mlme.h"
#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/platform/arm_hal_phy.h"
#include "mac_defines.h"
#include "mac_mlme.h"
#include "mac_lfn_async.h"
#include "rf_driver_storage.h"

static unsigned checks, failures;
#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* ------------------------------------------------------------- fake PHY */

static bool radio_on;           // after the last state change
static unsigned downs;          // PHY_INTERFACE_DOWN calls
static uint8_t rx_active;       // PHY_EXTENSION_READ_RX_ACTIVE
static uint8_t channel;

static int8_t fake_state_control(phy_interface_state_e state, uint8_t ch)
{
    if (state == PHY_INTERFACE_UP) {
        radio_on = true;
        channel = ch;
    } else if (state == PHY_INTERFACE_DOWN) {
        radio_on = false;
        downs++;
    }
    return 0;
}

static int8_t fake_extension(phy_extension_type_e type, uint8_t *data)
{
    switch (type) {
        case PHY_EXTENSION_READ_RX_ACTIVE:
            *data = rx_active;
            return 0;
        case PHY_EXTENSION_SET_CHANNEL:
            channel = *data;
            return 0;
        default:
            return 0;
    }
}

static phy_device_driver_s phy;
static arm_device_driver_list_s driver;
static protocol_interface_rf_mac_setup_s mac;

static void setup(void)
{
    memset(&phy, 0, sizeof(phy));
    memset(&driver, 0, sizeof(driver));
    memset(&mac, 0, sizeof(mac));
    phy.state_control = fake_state_control;
    phy.extension = fake_extension;
    driver.phy_driver = &phy;
    mac.dev_driver = &driver;
    mac.macUpState = true;
    mac.macCapRxOnIdle = false;     // an LFN: RX off when idle
    radio_on = false;
    downs = 0;
    rx_active = 0;
    channel = 0;
}

/* A data confirm: the MAC's own radio-off check (mac_mcps_sap.c calls it) */
static void data_confirm(void)
{
    mac_data_poll_radio_disable_check(&mac);
}

/* ------------------------------------------------------------------ tests */

static void test_window_open_close(void)
{
    setup();
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOWS_START, 0) == 0);
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_OPEN, 12) == 0);
    CHECK(radio_on && channel == 12);
    // An open window keeps the radio on through a data confirm
    data_confirm();
    CHECK(radio_on && downs == 0);
    // Closed with nothing received: off
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 0);
    CHECK(!radio_on && downs == 1);
}

static void test_deferred_close(void)
{
    setup();
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOWS_START, 0);
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_OPEN, 12);
    // The window's end comes during a frame: the close is deferred
    rx_active = 1;
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 1);
    CHECK(radio_on && downs == 0);
    // A data confirm meanwhile (a queued frame's timeout at its restart)
    // must not cut the frame. Before the fix the radio went off here.
    CHECK(mac_lfn_radio_stays_on(&mac));
    data_confirm();
    CHECK(radio_on && downs == 0);
    // Still receiving at the FHSS's next try: deferred again
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 1);
    data_confirm();
    CHECK(radio_on && downs == 0);
    // The frame has ended: the next try closes, the radio goes off, and a
    // later confirm turns it off again as usual
    rx_active = 0;
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 0);
    CHECK(!radio_on && downs == 1);
    CHECK(!mac_lfn_radio_stays_on(&mac));
}

static void test_deferred_close_then_open(void)
{
    setup();
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOWS_START, 0);
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_OPEN, 12);
    rx_active = 1;
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 1);
    // The FHSS gave up retrying (the next window was near) and opens it:
    // an open window, no deferred close any more
    rx_active = 0;
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_OPEN, 20) == 0);
    CHECK(mac.mac_rx_window_open && !mac.mac_rx_window_close_deferred);
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 0);
    CHECK(!radio_on);
    // Deferred, then the listen schedule stops: the usual rule again
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_OPEN, 12);
    rx_active = 1;
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 1);
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOWS_STOP, 0) == 0);
    CHECK(!mac.mac_rx_window_close_deferred && !mac_lfn_radio_stays_on(&mac));
}

static void test_outside_windows(void)
{
    // Outside any window (the radio on for a transmission and its ACK
    // wait) a confirm turns the radio off, a frame in progress or not
    // (unchanged: the MAC's no-ACK confirm, emulator test_lfn_left.sh)
    setup();
    mac_mlme_rx_window(&mac, FHSS_RX_WINDOWS_START, 0);
    mac_mlme_mac_radio_enable(&mac);
    rx_active = 1;
    CHECK(!mac_lfn_radio_stays_on(&mac));
    data_confirm();
    CHECK(!radio_on && downs == 1);
    // An FFN (RX on when idle) keeps it on
    setup();
    mac.macCapRxOnIdle = true;
    mac_mlme_mac_radio_enable(&mac);
    CHECK(mac_lfn_radio_stays_on(&mac));
    CHECK(mac_mlme_rx_window(&mac, FHSS_RX_WINDOW_CLOSE, 0) == 0);
    CHECK(radio_on && downs == 0);
}

int main(void)
{
    test_window_open_close();
    test_deferred_close();
    test_deferred_close_then_open();
    test_outside_windows();
    printf("mac_window: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
