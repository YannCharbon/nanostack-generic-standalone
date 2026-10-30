/*
 * Copyright (c) 2018-2021, Pelion and affiliates.
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
#ifndef FHSS_WS_H_
#define FHSS_WS_H_
#include "nanostack/mac/fhss_ws_extension.h"
#include "fhss_ws_lfn.h"

// TX slot length is optimised to this packet length
#define OPTIMAL_PACKET_LENGTH               500
// Max TX/RX slot length in milliseconds. Is used when datarate is not given by PHY or calculated slot length exceeds maximum allowed.
#define WS_TXRX_SLOT_LEN_MS_MAX             100
// Min TX/RX slot length in milliseconds. Is used when calculated slot length is under minimum allowed.
#define WS_TXRX_SLOT_LEN_MS_MIN             13
// Default minimum broadcast synchronization interval in seconds
#define DEFAULT_MIN_SYNCH_INTERVAL          60
// Drift compensation allowed if at least SYNCH_COMPENSATION_MIN_INTERVAL (seconds) since last synchronization
#define SYNCH_COMPENSATION_MIN_INTERVAL     60
// MAX compensation per received synchronization info in ns
#define MAX_DRIFT_COMPENSATION_STEP         10
// Length of expedited forwarding period in seconds
#define EXPEDITED_FORWARDING_PERIOD         5
// TX poll interval for expedited forwarding in 50us slots
#define EXPEDITED_FORWARDING_POLL_PERIOD    (5000 / 50)
// TX poll interval used when channel schedules are not yet started (50us slots)
#define DEFAULT_POLL_PERIOD    (10000 / 50)
typedef struct fhss_ws fhss_ws_t;

/* Wi-SUN FAN 1.1 LFN state (GS8 stages 2 and 4, docs/LFN_MAC.md). Both
 * roles off: the FHSS is the FAN 1.0 one. One FHSS timer callback serves the
 * unicast windows and the parent role (fhss_ws_lfn_timer_handler), another
 * the LFN's broadcast windows (fhss_ws_lfn_bc_timer_handler): at most four
 * distinct FHSS timer callbacks in all (unicast, broadcast, LFN, LFN
 * broadcast). */
struct fhss_ws_lfn {
    /* LFN role: own unicast listen schedule */
    bool listen_enabled;            // configured (ns_fhss_ws_lfn_listen_set)
    bool listen_running;            // started (FHSS synchronized)
    bool window_open;
    fhss_ws_lfn_listen_t listen_cfg;
    fhss_lfn_sched_t own;           // own schedule: anchor = slot 0 start
    uint16_t slot;                  // current (open) or next slot
    uint32_t slot_start;            // its start time
    uint8_t window_channel;
    uint16_t close_retries;
    bool resched;                   // LTO applied while a window was open: the next slot comes from 'own'
    /* LFN role: the parent's FFN broadcast schedule, kept clear by uplinks */
    bool parent_bc_valid;
    uint32_t parent_bc_start_us;    // start of one of its broadcast intervals
    uint32_t parent_bc_interval_us;
    uint32_t parent_bc_dwell_us;
    /* LFN role: the parent's LFN broadcast schedule (stage 4) */
    bool pbc_valid;                 // synced at least once
    fhss_lfn_sched_t pbc;           // its slots on our clock (interval corrected by rate_ppb)
    uint32_t pbc_nominal_us;        // its interval on the parent's clock
    uint16_t pbc_bsi;
    uint8_t pbc_sync_period;
    uint8_t pbc_channel_function;
    uint16_t pbc_fixed_channel;
    uint32_t sync_us;               // our time of the last sync: windows widen from it
    bool rate_ref_valid;            // a reference sync for the rate measurement
    uint32_t rate_ref_us;           // its slot start on our clock
    uint16_t rate_ref_slot;
    int32_t rate_ppb;               // our clock against the parent's
    bool bc_window_open;
    uint16_t bc_slot;               // current (open) or next broadcast slot
    uint32_t bc_slot_start;
    uint16_t bc_close_retries;
    bool resched_after_bc;          // a sync during a broadcast window: our next unicast slot is computed at its close
    /* LFN role: LND discovery window (stage 5), on the broadcast-window timer */
    bool disc_set;
    bool disc_open;
    uint32_t disc_start;
    uint32_t disc_end;
    uint8_t disc_channel;
    uint16_t disc_first_slot;       // LND-IE Discovery First Slot
    uint32_t disc_slot_us;          // LND slot length; 0: own nearest slot's channel
    bool tx_slot_valid;             // parent: the frame released at tx_dst_time goes on the LFN's channel of tx_slot
    uint16_t tx_slot;
    /* Parent role */
    bool parent_enabled;
    fhss_ws_lfn_parent_t parent_cfg;
    bool poll_armed;                // TX queue poll armed for a held frame
    uint32_t poll_time;             // its time
    bool bc_enabled;                // LFN broadcast schedule (stage 4)
    fhss_ws_lfn_bc_t bc_cfg;
    fhss_lfn_sched_t bc;            // its slots on our clock
    bool tx_dst_valid;              // the last frame released for an LFN window: destination, TX time
    uint8_t tx_dst[8];
    uint32_t tx_dst_time;
    bool lto_cache_valid;           // the LTO offset written in the last frame with an LTO-IE
    uint8_t lto_eui64[8];
    uint32_t lto_phase_ms;          // as the upper layer gave it
    uint32_t lto_offset_ms;         // as written (against the LFN's slot)
    fhss_ws_lfn_stats_t stats;
};

struct fhss_ws {
    uint8_t bc_channel;
    uint16_t uc_slot;
    uint16_t bc_slot;
    uint16_t min_synch_interval;
    uint32_t txrx_slot_length_ms;
    uint32_t synchronization_time;
    uint32_t unicast_start_time_us;
    int32_t drift_per_millisecond_ns;
    int16_t *tr51_channel_table;
    uint8_t *tr51_output_table;
    uint32_t next_uc_timeout;
    uint32_t next_bc_timeout;
    uint32_t expedited_forwarding_enabled_us;
    bool unicast_timer_running;
    bool broadcast_timer_running;
    bool is_on_bc_channel;
    fhss_ws_tx_allow_level tx_level;
    fhss_ws_tx_allow_level ef_tx_level;
    struct fhss_ws_configuration fhss_configuration;
    fhss_get_neighbor_info *get_neighbor_info;
    struct fhss_ws_lfn lfn;
};

fhss_structure_t *fhss_ws_enable(fhss_api_t *fhss_api, const fhss_ws_configuration_t *fhss_configuration, const fhss_timer_t *fhss_timer);
int fhss_ws_set_callbacks(fhss_structure_t *fhss_structure);
int fhss_ws_set_parent(fhss_structure_t *fhss_structure, const uint8_t eui64[8], const broadcast_timing_info_t *bc_timing_info, const bool force_synch);
int fhss_ws_remove_parent(fhss_structure_t *fhss_structure, const uint8_t eui64[8]);
int fhss_ws_configuration_set(fhss_structure_t *fhss_structure, const fhss_ws_configuration_t *fhss_configuration);
int fhss_ws_set_hop_count(fhss_structure_t *fhss_structure, const uint8_t hop_count);
int fhss_ws_set_tx_allowance_level(fhss_structure_t *fhss_structure, const fhss_ws_tx_allow_level global_level, const fhss_ws_tx_allow_level ef_level);
void fhss_set_txrx_slot_length(fhss_structure_t *fhss_structure);
int fhss_ws_lfn_listen_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_listen_t *cfg);
int fhss_ws_lfn_parent_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_parent_t *cfg);
int fhss_ws_lfn_timing_set(fhss_structure_t *fhss_structure, uint32_t offset_ms, uint32_t interval_ms, uint32_t rx_timestamp_us);
int fhss_ws_lfn_neighbor_lto(fhss_structure_t *fhss_structure, const uint8_t eui64[8], uint32_t offset_ms, uint32_t interval_ms, uint32_t tx_timestamp_us);
int fhss_ws_lfn_bc_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_bc_t *cfg);
int fhss_ws_lfn_bc_next(fhss_structure_t *fhss_structure, uint16_t *slot, uint32_t *start_us);
int fhss_ws_lfn_bc_sync(fhss_structure_t *fhss_structure, const fhss_ws_lfn_bc_sync_t *sync);
int fhss_ws_lfn_bc_stop(fhss_structure_t *fhss_structure);
int fhss_ws_lfn_discovery_set(fhss_structure_t *fhss_structure, uint32_t start_us, uint32_t duration_us, uint16_t first_slot, uint32_t slot_us);

#endif /*FHSS_WS_H_*/
