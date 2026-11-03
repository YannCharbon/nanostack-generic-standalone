/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MAC_LFN_ASYNC_H_
#define MAC_LFN_ASYNC_H_

#include <stdbool.h>
#include <stdint.h>

struct fhss_api;
struct mac_pre_build_frame;

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

#endif
