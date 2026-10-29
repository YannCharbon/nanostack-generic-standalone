/*
 * Copyright (c) 2018, 2020-2021, Pelion and affiliates.
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
#ifndef FHSS_WS_EXT_H
#define FHSS_WS_EXT_H
#include "mbed-client-libservice/ns_types.h"

/**
 * \file fhss_ws_extension.h
 * \brief
 */

typedef struct fhss_api fhss_api_t;


/**
 * @brief ws_channel_mask_t WS neighbour supported channel mask
 */
typedef struct ws_channel_mask {
    uint16_t channel_count;                     /**<active channels at mask */
    uint32_t channel_mask[8];                   /**< Supported channels */
} ws_channel_mask_t;

/**
 * @brief unicast_timing_info Unicast timing/hopping schedule information structure.
 */
typedef struct unicast_timing_info {
    unsigned unicast_channel_function: 3;       /**< Unicast schedule channel function */
    uint8_t unicast_dwell_interval;             /**< Unicast dwell interval */
    uint16_t unicast_number_of_channels;        /**< Unicast number of channels */
    uint16_t fixed_channel;                     /**< Unicast fixed channel*/
    uint_fast24_t ufsi;                         /**< Unicast fractional sequence interval */
    uint32_t utt_rx_timestamp;                  /**< UTT-IE reception timestamp */
} unicast_timing_info_t;

/**
 * @brief broadcast_timing_info Broadcast timing/hopping schedule information structure.
 */
typedef struct broadcast_timing_info {
    unsigned broadcast_channel_function: 3;     /**< Broadcast schedule channel function */
    uint8_t broadcast_dwell_interval;           /**< Broadcast dwell interval */
    uint16_t fixed_channel;                     /**< Broadcast fixed channel*/
    uint16_t broadcast_slot;                    /**< Broadcast slot number */
    uint16_t broadcast_schedule_id;             /**< Broadcast schedule identifier */
    uint_fast24_t broadcast_interval_offset;    /**< Broadcast interval offset */
    uint32_t broadcast_interval;                /**< Broadcast interval */
    uint32_t bt_rx_timestamp;                   /**< BT-IE reception timestamp */
} broadcast_timing_info_t;

/**
 * @brief lfn_unicast_timing_info LFN unicast listen schedule of a neighbour
 * (Wi-SUN FAN 1.1, GS8): what an LFN parent needs to reach the LFN in its
 * listen windows. Filled by the upper layer from the LFN's LUS-IE and
 * LUTT-IE. listen_interval_ms 0 means the neighbour is not an LFN (FAN 1.0
 * neighbours keep it 0). The LFN's channel function and channel list are
 * those of uc_timing_info and uc_channel_list.
 */
typedef struct lfn_unicast_timing_info {
    uint32_t listen_interval_ms;                /**< LUS-IE listen interval (ms); 0: no LFN schedule */
    uint16_t slot_number;                       /**< LUTT-IE unicast slot number */
    uint_fast24_t interval_offset_ms;           /**< LUTT-IE unicast interval offset (ms into the slot) */
    uint32_t lutt_rx_timestamp;                 /**< LUTT-IE reception timestamp (µs) */
} lfn_unicast_timing_info_t;

/**
 * @brief fhss_ws_neighbor_timing_info Neighbor timing/hopping schedule information structure.
 */
typedef struct fhss_ws_neighbor_timing_info {
    uint8_t clock_drift;                        /**< Neighbor clock drift */
    uint8_t timing_accuracy;                    /**< Neighbor timing accuracy */
    unicast_timing_info_t uc_timing_info;       /**< Neighbor unicast timing info */
    broadcast_timing_info_t bc_timing_info;     /**< Neighbor broadcast timing info */
    ws_channel_mask_t uc_channel_list;          /**< Neighbor Unicast channel list */
    uint32_t *excluded_channels;                /**< Neighbor excluded channels (bit mask) */
    lfn_unicast_timing_info_t lfn_uc_timing_info; /**< Neighbor LFN unicast schedule (FAN 1.1); zero for an FFN */
} fhss_ws_neighbor_timing_info_t;

/**
 * @brief Get neighbor timing/hopping schedule.
 * @param api FHSS instance.
 * @param eui64 EUI-64 address of node for which the info is requested.
 * @return Neighbor timing/hopping schedule.
 */
typedef fhss_ws_neighbor_timing_info_t *fhss_get_neighbor_info(const fhss_api_t *api, uint8_t eui64[8]);

/**
 * @brief Set parent which broadcast channels must be listened by FHSS.
 * @param fhss_api FHSS instance.
 * @param eui64 EUI-64 address of parent.
 * @param bc_timing_info Pointer to parent broadcast timing/hopping schedule info.
 * @param force_synch If false, synchronization is done only if minimum (internal) synchronization interval is exceed.
 * @return 0 on success, -1 on fail.
 */
extern int ns_fhss_ws_set_parent(const fhss_api_t *fhss_api, const uint8_t eui64[8], const broadcast_timing_info_t *bc_timing_info, const bool force_synch);

/**
 * @brief Remove parent which was set by ns_fhss_ws_set_parent function.
 * @param fhss_api FHSS instance.
 * @param eui64 EUI-64 address of parent.
 * @return 0 on success, -1 on fail.
 */
extern int ns_fhss_ws_remove_parent(const fhss_api_t *fhss_api, const uint8_t eui64[8]);

/* @brief Update the MAC layer with the new timing information about a neighbor.
 *     This function is not called by the original nanostack. It is used to push
 *     relevant information to the host in the case of a splited stack.
 * @param eui64 MAC adress of the remote host
 * @param fhss_data Pointer to timing information
 */
extern void ns_fhss_ws_update_neighbor(const uint8_t eui64[8], fhss_ws_neighbor_timing_info_t *fhss_data);


/**
 * @brief Inform the MAC layer it can drop a neighbor from its list. This
 *     function is not called by the original nanostack. It is used to push
 *     relevant information to the host in the case of a splited stack.
 * @param eui64 MAC adress of the remote host
 */
extern void ns_fhss_ws_drop_neighbor(const uint8_t eui64[8]);

/**
 * @brief Set neighbor timing/hopping schedule request function.
 * @param fhss_api FHSS instance.
 * @param get_neighbor_info Neighbor info function pointer.
 * @return 0 on success, -1 on fail.
 */
extern int ns_fhss_set_neighbor_info_fp(const fhss_api_t *fhss_api, fhss_get_neighbor_info *get_neighbor_info);

/**
 * @brief Set node hop count. Hop count is used to specify TX/RX slot. When hop count is set to 0xFF, TX/RX slots are ignored.
 * @param fhss_api FHSS instance.
 * @param hop_count Hop count to be set.
 * @return 0 on success, -1 on fail.
 */
extern int ns_fhss_ws_set_hop_count(const fhss_api_t *fhss_api, const uint8_t hop_count);

/**
 * @brief WS TX allowance levels.
 */
typedef enum {
    /** Allow transmitting only on TX slots. */
    WS_TX_SLOT,
    /** Allow transmitting only on TX and RX slots. */
    WS_TX_AND_RX_SLOT,
    /** Allow transmitting always. Also unicast on broadcast channel. */
    WS_TX_ALWAYS
} fhss_ws_tx_allow_level;

/**
 * @brief Set node unicast TX allowance level. Allows device to use the unicast and broadcast channel for unicast transmission as described by fhss_ws_tx_allow_level.
 * @param fhss_api FHSS instance.
 * @param global_level Level of TX allowance in normal mode.
 * @param ef_level Level of TX allowance in expedited forwarding mode.
 * @return 0 on success, -1 on fail.
 */
extern int ns_fhss_ws_set_tx_allowance_level(const fhss_api_t *fhss_api, const fhss_ws_tx_allow_level global_level, const fhss_ws_tx_allow_level ef_level);

/*
 * Wi-SUN FAN 1.1 LFN unicast mechanics in the FHSS (GS8 stage 2; design in
 * docs/LFN_MAC.md). Both roles are off by default, and with both off the
 * FHSS behaves exactly as before (FAN 1.0). All times are user settings;
 * the defaults below are only starting points.
 */

/** Defaults of fhss_ws_lfn_listen_t (ns_fhss_ws_lfn_listen_defaults()). */
#define FHSS_LFN_LISTEN_INTERVAL_MS_DEFAULT     1000u   /**< Listen interval */
#define FHSS_LFN_RX_WINDOW_US_DEFAULT           5000u   /**< Receive window length */
#define FHSS_LFN_RX_GUARD_US_DEFAULT            1000u   /**< Window opens this long before the slot start */
#define FHSS_LFN_CLOSE_RETRY_US_DEFAULT         2000u   /**< Retry period of a close the MAC deferred (frame being received or ACK being sent) */
#define FHSS_LFN_FIRST_SLOT_DELAY_US_DEFAULT    10000u  /**< Start of slot 0 after the schedule starts */
#define FHSS_LFN_UPLINK_GUARD_US_DEFAULT        5000u   /**< Time kept clear of the parent's broadcast dwell after an uplink frame starts (frame, ACK) beyond the frame's airtime */

/**
 * @brief LFN role: the node's own unicast listen schedule (LUS-IE). The
 * receiver is on only in a window around the start of each slot, on the
 * channel of the node's unicast channel function for that slot number:
 * [start - rx_guard_us, start - rx_guard_us + rx_window_us). Outside the
 * windows the radio is off, except to transmit (and wait for the ACK).
 */
typedef struct fhss_ws_lfn_listen {
    uint32_t listen_interval_ms;    /**< Listen interval (ms), 1 .. 2147483 (32-bit clock, LFN study M11) */
    uint32_t rx_window_us;          /**< Window length (µs), 1 .. interval */
    uint32_t rx_guard_us;           /**< Window opening before the slot start (µs), < rx_window_us */
    uint32_t close_retry_us;        /**< Retry period of a deferred close (µs), >= 1 */
    uint32_t first_slot_delay_us;   /**< Slot 0 starts this long after the schedule starts (µs), > rx_guard_us */
    uint32_t uplink_guard_us;       /**< Uplink: time after the frame's airtime kept clear of the parent's broadcast dwell (µs; ACK turnaround and airtime) */
} fhss_ws_lfn_listen_t;

/** Defaults of fhss_ws_lfn_parent_t (ns_fhss_ws_lfn_parent_defaults()). */
#define FHSS_LFN_TX_OFFSET_US_DEFAULT           0u      /**< TX start after the LFN's slot start */
#define FHSS_LFN_TX_LEAD_US_DEFAULT             20000u  /**< Frame released to the MAC this long before its TX time */
#define FHSS_LFN_TX_MIN_PREP_US_DEFAULT         2000u   /**< Shortest time from release to TX; a closer window is skipped */
#define FHSS_LFN_TX_TTL_INTERVALS_DEFAULT       20u     /**< Frame dropped after waiting this many listen intervals */

/**
 * @brief LFN parent role: unicast frames to a neighbour with an LFN unicast
 * schedule (lfn_unicast_timing_info_t) are held until its next listen slot
 * and sent at slot start + tx_offset_us, on the LFN's channel for that slot,
 * whatever the parent's own TX/RX slots and broadcast dwell. A failed attempt
 * (no ACK, busy channel, too late) waits for the next slot; the MAC's retry
 * limits still apply.
 */
typedef struct fhss_ws_lfn_parent {
    uint32_t tx_offset_us;          /**< TX start after the LFN's slot start (µs) */
    uint32_t tx_lead_us;            /**< Release this long before the TX time (µs), <= 60000 (PHY TX time limit) */
    uint32_t tx_min_prep_us;        /**< Shortest release-to-TX time (µs), <= tx_lead_us */
    uint16_t tx_ttl_intervals;      /**< Frame TTL in the LFN's listen intervals, >= 1 */
} fhss_ws_lfn_parent_t;

/**
 * @brief Counters of the LFN mechanics (ns_fhss_ws_lfn_stats_get()).
 */
typedef struct fhss_ws_lfn_stats {
    uint32_t windows_opened;        /**< LFN: receive windows opened */
    uint32_t windows_skipped;       /**< LFN: slots skipped because the FHSS timer fired late */
    uint32_t closes_deferred;       /**< LFN: window closes the MAC deferred (receiving, sending an ACK) */
    uint32_t tx_released;           /**< Parent: frames released for an LFN window */
    uint32_t tx_held;               /**< Parent: checks that held a frame for a later window */
    uint32_t tx_expired;            /**< Parent: frames dropped after their TTL */
    uint32_t uplink_deferred;       /**< LFN: uplink frames held until the parent's broadcast dwell ended */
} fhss_ws_lfn_stats_t;

/** Fill *cfg with the FHSS_LFN_*_DEFAULT values. */
extern void ns_fhss_ws_lfn_listen_defaults(fhss_ws_lfn_listen_t *cfg);
extern void ns_fhss_ws_lfn_parent_defaults(fhss_ws_lfn_parent_t *cfg);

/**
 * @brief Start (cfg) or stop (NULL) the LFN unicast listen schedule.
 *
 * While it runs, the FFN unicast hopping and the FFN broadcast schedule are
 * stopped, ns_fhss_ws_set_parent() is ignored, and the TX queue is not polled
 * periodically. The MAC should have macRxOnWhenIdle false. If the FHSS is not
 * synchronized yet (MLME-START), the schedule starts when it is. Stopping it
 * restarts the FFN unicast hopping.
 * @return 0 OK, -1 no FHSS or the MAC has no timed windows, -2 invalid cfg,
 *         -3 the LFN parent role is on.
 */
extern int ns_fhss_ws_lfn_listen_set(const fhss_api_t *fhss_api, const fhss_ws_lfn_listen_t *cfg);

/**
 * @brief Enable (cfg) or disable (NULL) the LFN parent role.
 * @return 0 OK, -1 no FHSS, -2 invalid cfg, -3 the LFN listen schedule runs.
 */
extern int ns_fhss_ws_lfn_parent_set(const fhss_api_t *fhss_api, const fhss_ws_lfn_parent_t *cfg);

/**
 * @brief LFN role: start the configured listen schedule now, as MLME-START
 * would with a PAN ID: an LFN listens for its parent's answers before it
 * knows its PAN (GS8 stage 3). The FHSS is then synchronized.
 * @return 0 OK, -1 no FHSS or no listen schedule configured.
 */
extern int ns_fhss_ws_lfn_listen_start(const fhss_api_t *fhss_api);

/**
 * @brief LFN role: apply an LTO-IE
 (LFN Timing Offset) received at
 * rx_timestamp_us. The slot that contains rx_timestamp_us and the slots after
 * it start offset_ms later, and the listen interval becomes interval_ms (0:
 * unchanged). An open window stays open; the next one follows the new
 * schedule. Also used with offset 0 to change the interval alone (for
 * example during EAPOL). docs/LFN_MAC.md §2.1.
 * @return 0 OK, -1 no FHSS or not an LFN, -2 invalid interval.
 */
extern int ns_fhss_ws_lfn_timing_set(const fhss_api_t *fhss_api, uint32_t offset_ms, uint32_t interval_ms, uint32_t rx_timestamp_us);

/**
 * @brief Parent role: the LFN's schedule after an LTO-IE that the parent sent
 * to it at tx_timestamp_us (in one of its windows) was acknowledged. Updates
 * the neighbour's lfn_uc_timing_info as fhss_ws_lfn_timing_set() moves the
 * LFN's own schedule.
 * @return 0 OK, -1 no FHSS, not a parent or no such LFN neighbour.
 */
extern int ns_fhss_ws_lfn_neighbor_lto(const fhss_api_t *fhss_api, const uint8_t eui64[8], uint32_t offset_ms, uint32_t interval_ms, uint32_t tx_timestamp_us);

/** @brief Read the LFN counters. @return 0 OK, -1 no FHSS. */
extern int ns_fhss_ws_lfn_stats_get(const fhss_api_t *fhss_api, fhss_ws_lfn_stats_t *stats);

#endif // FHSS_WS_EXT_H
