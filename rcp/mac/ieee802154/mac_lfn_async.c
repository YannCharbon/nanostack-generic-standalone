/* SPDX-License-Identifier: Apache-2.0
 *
 * FAN 1.1 parent: asynchronous sweeps (PA, PC on every channel: 0.3-0.6 s)
 * and frames queued for LFN children. A border router's PAN configuration
 * sweep took an LFN's window (the frame then came one listen interval later)
 * and the 10 h sync slot of the Eco profile [I]; wisun-br-linux leaves frame
 * timing to its RCP. Kept apart from mac_mcps_sap.c for its unit test
 * (tests/lfn_units).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nanostack/mac/mlme.h"
#include "nanostack/mac/fhss_api.h"
#include "mac_data_buffer.h"
#include "mac_lfn_async.h"

uint32_t mac_lfn_async_sweep_us(const struct mac_pre_build_frame *async, uint32_t datarate)
{
    uint32_t channels = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t m = async->asynch_channel_list.channel_mask[i];
        while (m) {
            m &= m - 1;
            channels++;
        }
    }
    uint32_t bytes = async->mac_payload_length + async->headerIeLength + async->payloadsIeLength + 40;
    uint32_t per_channel = (uint32_t)((uint64_t)bytes * 8 * 1000000 / (datarate ? datarate : 50000)) + 3000;
    return channels * per_channel + MAC_LFN_ASYNC_MARGIN_US;
}

static bool mac_lfn_async_due(const struct fhss_api *fhss_api, const struct mac_pre_build_frame *b, uint32_t now, uint32_t sweep_us)
{
    for (; b; b = b->next) {
        uint32_t delay;
        if (b->asynch_request) {
            continue;
        }
        if (b->lfn_req_set) {
            // an LPA at a requested time
            delay = (int32_t)(b->lfn_req_time - now) > 0 ? b->lfn_req_time - now : 0;
        } else if (b->lfn_bc) {
            if (!fhss_api->lfn_tx_delay(fhss_api, NULL, &delay)) {
                continue;
            }
        } else if ((b->fcf_dsn.ackRequested || b->WaitResponse) && b->fcf_dsn.DstAddrMode == MAC_ADDR_MODE_64_BIT) {
            if (!fhss_api->lfn_tx_delay(fhss_api, b->DstAddr, &delay)) {
                continue;
            }
        } else {
            continue;
        }
        if (delay < sweep_us) {
            return true;
        }
    }
    return false;
}

bool mac_lfn_async_hold(const struct fhss_api *fhss_api, const struct mac_pre_build_frame *async,
                        const struct mac_pre_build_frame *queue, const struct mac_pre_build_frame *bc_queue,
                        uint32_t now, uint32_t datarate)
{
    if (!async->asynch_request || !fhss_api || !fhss_api->lfn_tx_delay) {
        return false;
    }
    if (now - async->request_start_time_us > MAC_LFN_ASYNC_DEFER_MAX_US) {
        return false;
    }
    uint32_t sweep_us = mac_lfn_async_sweep_us(async, datarate);
    return mac_lfn_async_due(fhss_api, queue, now, sweep_us) || mac_lfn_async_due(fhss_api, bc_queue, now, sweep_us);
}
