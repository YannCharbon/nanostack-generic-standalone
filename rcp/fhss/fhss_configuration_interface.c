/*
 * Copyright (c) 2016-2019, 2021, Pelion and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>
#include <stdint.h>
#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_config.h"
#include "nanostack/mac/net_fhss.h"
#include <stdlib.h>
#include "fhss.h"
#include "fhss_common.h"
#include "fhss_ws.h"
#include "fhss_statistics.h"
#include "mbed-client-libservice/ns_trace.h"

#define TRACE_GROUP "fhss"

fhss_api_t *ns_fhss_ws_create(const fhss_ws_configuration_t *fhss_configuration, const fhss_timer_t *fhss_timer)
{
    fhss_api_t *this = malloc(sizeof(fhss_api_t));
    if (!this) {
        return NULL;
    }
    // Create FHSS object
    fhss_structure_t *fhss_struct = fhss_ws_enable(this, fhss_configuration, fhss_timer);
    if (!fhss_struct) {
        tr_err("Failed to enable FHSS");
        free(this);
        return NULL;
    }
    fhss_ws_set_callbacks(fhss_struct);
    return this;
}

int ns_fhss_ws_set_parent(const fhss_api_t *fhss_api, const uint8_t eui64[8], const broadcast_timing_info_t *bc_timing_info, const bool force_synch)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !eui64 || !bc_timing_info) {
        return -1;
    }
    return fhss_ws_set_parent(fhss_structure, eui64, bc_timing_info, force_synch);
}

int ns_fhss_ws_remove_parent(const fhss_api_t *fhss_api, const uint8_t eui64[8])
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !eui64) {
        return -1;
    }
    return fhss_ws_remove_parent(fhss_structure, eui64);
}

void ns_fhss_ws_update_neighbor(const uint8_t eui64[8], fhss_ws_neighbor_timing_info_t *fhss_data)
{
    (void)eui64;
    (void)fhss_data;
}

void ns_fhss_ws_drop_neighbor(const uint8_t eui64[8])
{
    (void)eui64;
}

int ns_fhss_delete(fhss_api_t *fhss_api)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure) {
        return -1;
    }
    if (fhss_disable(fhss_structure)) {
        return -1;
    }
    free(fhss_api);
    return 0;
}

int ns_fhss_set_neighbor_info_fp(const fhss_api_t *fhss_api, fhss_get_neighbor_info *get_neighbor_info)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    fhss_structure->ws->get_neighbor_info = get_neighbor_info;
    return 0;
}

const fhss_ws_configuration_t *ns_fhss_ws_configuration_get(const fhss_api_t *fhss_api)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return NULL;
    }
    return &fhss_structure->ws->fhss_configuration;
}

int ns_fhss_ws_configuration_set(const fhss_api_t *fhss_api, const fhss_ws_configuration_t *fhss_configuration)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_configuration_set(fhss_structure, fhss_configuration);
}

int ns_fhss_ws_set_hop_count(const fhss_api_t *fhss_api, const uint8_t hop_count)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_set_hop_count(fhss_structure, hop_count);
}

int ns_fhss_ws_set_tx_allowance_level(const fhss_api_t *fhss_api, const fhss_ws_tx_allow_level global_level, const fhss_ws_tx_allow_level ef_level)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_set_tx_allowance_level(fhss_structure, global_level, ef_level);
}

void ns_fhss_ws_lfn_listen_defaults(fhss_ws_lfn_listen_t *cfg)
{
    cfg->listen_interval_ms = FHSS_LFN_LISTEN_INTERVAL_MS_DEFAULT;
    cfg->rx_window_us = FHSS_LFN_RX_WINDOW_US_DEFAULT;
    cfg->rx_guard_us = FHSS_LFN_RX_GUARD_US_DEFAULT;
    cfg->close_retry_us = FHSS_LFN_CLOSE_RETRY_US_DEFAULT;
    cfg->first_slot_delay_us = FHSS_LFN_FIRST_SLOT_DELAY_US_DEFAULT;
    cfg->uplink_guard_us = FHSS_LFN_UPLINK_GUARD_US_DEFAULT;
    cfg->drift_ppm = FHSS_LFN_DRIFT_PPM_DEFAULT;
    cfg->widening_max_us = FHSS_LFN_WIDENING_MAX_US_DEFAULT;
    cfg->bc_rx_window_us = FHSS_LFN_BC_RX_WINDOW_US_DEFAULT;
    cfg->bc_sync_only = false;
}

void ns_fhss_ws_lfn_parent_defaults(fhss_ws_lfn_parent_t *cfg)
{
    cfg->tx_offset_us = FHSS_LFN_TX_OFFSET_US_DEFAULT;
    cfg->tx_lead_us = FHSS_LFN_TX_LEAD_US_DEFAULT;
    cfg->tx_min_prep_us = FHSS_LFN_TX_MIN_PREP_US_DEFAULT;
    cfg->tx_ttl_intervals = FHSS_LFN_TX_TTL_INTERVALS_DEFAULT;
}

int ns_fhss_ws_lfn_listen_set(const fhss_api_t *fhss_api, const fhss_ws_lfn_listen_t *cfg)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_listen_set(fhss_structure, cfg);
}

int ns_fhss_ws_lfn_parent_set(const fhss_api_t *fhss_api, const fhss_ws_lfn_parent_t *cfg)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_parent_set(fhss_structure, cfg);
}

int ns_fhss_ws_lfn_timing_set(const fhss_api_t *fhss_api, uint32_t offset_ms, uint32_t interval_ms, uint32_t rx_timestamp_us)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_timing_set(fhss_structure, offset_ms, interval_ms, rx_timestamp_us);
}

int ns_fhss_ws_lfn_neighbor_lto(const fhss_api_t *fhss_api, const uint8_t eui64[8], uint32_t offset_ms, uint32_t interval_ms, uint32_t tx_timestamp_us)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_neighbor_lto(fhss_structure, eui64, offset_ms, interval_ms, tx_timestamp_us);
}

int ns_fhss_ws_lfn_bc_set(const fhss_api_t *fhss_api, const fhss_ws_lfn_bc_t *cfg)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_bc_set(fhss_structure, cfg);
}

int ns_fhss_ws_lfn_bc_next(const fhss_api_t *fhss_api, uint16_t *slot, uint32_t *start_us)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_bc_next(fhss_structure, slot, start_us);
}

int ns_fhss_ws_lfn_bc_sync(const fhss_api_t *fhss_api, const fhss_ws_lfn_bc_sync_t *sync)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws || !sync) {
        return -1;
    }
    return fhss_ws_lfn_bc_sync(fhss_structure, sync);
}

int ns_fhss_ws_lfn_bc_stop(const fhss_api_t *fhss_api)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws) {
        return -1;
    }
    return fhss_ws_lfn_bc_stop(fhss_structure);
}

int ns_fhss_ws_lfn_stats_get(const fhss_api_t *fhss_api, fhss_ws_lfn_stats_t *stats)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws || !stats) {
        return -1;
    }
    *stats = fhss_structure->ws->lfn.stats;
    return 0;
}

int ns_fhss_ws_lfn_listen_start(const fhss_api_t *fhss_api)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws || !fhss_structure->ws->lfn.listen_enabled) {
        return -1;
    }
    if (fhss_structure->fhss_state != FHSS_SYNCHRONIZED) {
        fhss_api->synch_state_set(fhss_api, FHSS_SYNCHRONIZED, 0xffff);
    }
    return 0;
}
