/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MAC_LFN_ASYNC_H_
#define MAC_LFN_ASYNC_H_

#include <stdbool.h>
#include <stdint.h>

struct fhss_api;
struct mac_pre_build_frame;
struct protocol_interface_rf_mac_setup;

#define MAC_LFN_ASYNC_DEFER_MAX_US 10000000u   /**< An asynchronous sweep waits at most this long for LFN frames */
#define MAC_LFN_ASYNC_MARGIN_US    20000u      /**< Added to the sweep's estimated length */

/**
 * FAN 1.1 parent: would the asynchronous sweep 'async' (one frame per
 * channel of its list) hold the radio through the TX time of a frame queued
 * for an LFN in 'queue' or 'bc_queue' (its window, an LFN broadcast slot, an
 * LPA at its time)? Then the sweep waits (true), at most
 * MAC_LFN_ASYNC_DEFER_MAX_US after its request: the LFN frame goes first and
 * the queue is read again at its confirmation [I].
 */
bool mac_lfn_async_hold(const struct fhss_api *fhss_api, const struct mac_pre_build_frame *async,
                        const struct mac_pre_build_frame *queue, const struct mac_pre_build_frame *bc_queue,
                        uint32_t now, uint32_t datarate);

/** The estimated length of the asynchronous sweep 'async', µs: per channel its airtime + 3 ms, + MAC_LFN_ASYNC_MARGIN_US */
uint32_t mac_lfn_async_sweep_us(const struct mac_pre_build_frame *async, uint32_t datarate);

/**
 * After a data exchange (its confirm), does the radio stay on? With
 * macRxOnWhenIdle, while waiting for polled data, in an open LFN receive
 * window, and while a window's close waits for the frame being received
 * (FHSS_RX_WINDOW_CLOSE returned 1; the FHSS closes it again shortly): a
 * confirm then, such as a queued frame's timeout, cut that frame (GS11
 * follow-up, emulator test_lfn_left.sh) [I].
 */
bool mac_lfn_radio_stays_on(const struct protocol_interface_rf_mac_setup *rf_mac_setup);

/**
 * At a data confirm (and an LFN's frame waiting for its restart): the radio
 * off unless mac_lfn_radio_stays_on() or a transmission holds it. Here for
 * its unit test (tests/mac_window); called by mac_mcps_sap.c.
 */
void mac_data_poll_radio_disable_check(struct protocol_interface_rf_mac_setup *rf_mac_setup);

#endif
