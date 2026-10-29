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

/**
 * \file fhss_api.h
 * \brief
 */

#ifndef FHSS_API_H
#define FHSS_API_H
#include <stdint.h>
#include <stdbool.h>

typedef struct fhss_api fhss_api_t;
typedef struct fhss_callback fhss_callback_t;

/**
 * @brief FHSS frame types.
 */
#define FHSS_SYNCH_FRAME            0   /**< FHSS synchronization frame */
#define FHSS_SYNCH_REQUEST_FRAME    1   /**< FHSS synchronization request frame */
#define FHSS_DATA_FRAME             2   /**< FHSS data frame */
#define FHSS_LFN_BC_FRAME           3   /**< FAN 1.1: a frame for the parent's LFN broadcast slot (mcps_data_req_t.LfnBroadcast) */

/**
 * @brief FHSS synchronization info length.
 */
#define FHSS_SYNCH_INFO_LENGTH      21

/**
 * @brief FHSS states.
 */
typedef enum {
    FHSS_UNSYNCHRONIZED,
    FHSS_SYNCHRONIZED,
    FHSS_EXPEDITED_FORWARDING
} fhss_states;

/**
 * @brief FHSS is broadcast channel. Checks if current channel is broadcast channel.
 * @param api FHSS instance.
 * @return false if unicast channel, true if broadcast channel.
 */
typedef bool fhss_is_broadcast_channel(const fhss_api_t *api);

/**
 * @brief FHSS queue check. Checks if broadcast queue must be used instead of unicast queue.
 * @param api FHSS instance.
 * @param is_broadcast_addr Destination address type of packet (true if broadcast address).
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 * @return false if unicast queue, true if broadcast queue.
 */
typedef bool fhss_use_broadcast_queue(const fhss_api_t *api, bool is_broadcast_addr, int frame_type);

/**
 * @brief FHSS TX handle. Set destination channel and write synchronization info.
 * @param api FHSS instance.
 * @param is_broadcast_addr Destination address type of packet (true if broadcast address).
 * @param destination_address Destination MAC address.
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 * @param frame_length MSDU length of the frame.
 * @param phy_header_length PHY header length.
 * @param phy_tail_length PHY tail length.
 * @param tx_time TX time.
 * @return  0 Success.
 * @return -1 Transmission of the packet is currently not allowed, try again.
 * @return -2 Invalid api.
 * @return -3 Broadcast packet on Unicast channel (not allowed), push packet back to queue.
 */
typedef int fhss_tx_handle(const fhss_api_t *api, bool is_broadcast_addr, uint8_t *destination_address, int frame_type, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length, uint32_t tx_time);

/**
 * @brief Check TX permission.
 * @param api FHSS instance.
 * @param is_broadcast_addr Destination address type of packet (true if broadcast address).
 * @param handle Handle of the data request.
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 * @param frame_length MSDU length of the frame.
 * @param phy_header_length PHY header length.
 * @param phy_tail_length PHY tail length.
 * @return false if transmission is denied, true if transmission is allowed.
 */
typedef bool fhss_check_tx_conditions(const fhss_api_t *api, bool is_broadcast_addr, uint8_t handle, int frame_type, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length);

/**
 * @brief Notification of received FHSS synch or synch request frame.
 * @param api FHSS instance.
 * @param pan_id Source PAN id of the received frame (FHSS_SYNCH_FRAME only).
 * @param source_address Source address of the received frame (FHSS_SYNCH_FRAME only).
 * @param timestamp Timestamp of reception (FHSS_SYNCH_FRAME only).
 * @param synch_info Pointer to synchronization info (FHSS_SYNCH_FRAME only).
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 */
typedef void fhss_receive_frame(const fhss_api_t *api, uint16_t pan_id, uint8_t *source_address, uint32_t timestamp, uint8_t *synch_info, int frame_type);

/**
 * @brief Data TX done callback.
 * @param api FHSS instance.
 * @param waiting_ack MAC is waiting Acknowledgement for this frame.
 * @param tx_completed TX completed (Ack received or no more retries left (if unicast frame)).
 * @param handle Handle of the data request.
 */
typedef void fhss_data_tx_done(const fhss_api_t *api, bool waiting_ack, bool tx_completed, uint8_t handle);

/**
 * @brief Data TX or CCA failed callback.
 * @param api FHSS instance.
 * @param handle Handle of the data request.
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 * @param channel Channel wanted to black list temporarily.
 * @return true if frame has to be queued for retransmission, false otherwise.
 */
typedef bool fhss_data_tx_fail(const fhss_api_t *api, uint8_t handle, int frame_type, uint8_t channel);

/**
 * @brief Change synchronization state.
 * @param api FHSS instance.
 * @param fhss_state FHSS state (FHSS states are defined by FHSS api).
 * @param pan_id PAN id of the network FHSS synchronizes with.
 * @return -1 when failed, otherwise current MAC channel.
 */
typedef int16_t fhss_synch_state_set(const fhss_api_t *api, fhss_states fhss_state, uint16_t pan_id);

/**
 * @brief Read timestamp.
 * @param api FHSS instance.
 * @return Timestamp to be written in received frame.
 */
typedef uint32_t fhss_read_timestamp(const fhss_api_t *api);

/**
 * @brief Get additional retransmission period. FHSS uses different retry periods depending on destination or channel availability.
 * @param api FHSS instance.
 * @param destination_address Destination MAC address.
 * @param phy_mtu PHY MTU size.
 * @return Retransmission period in microsecond which should be added to normal backoff period.
 */
typedef uint32_t fhss_get_retry_period(const fhss_api_t *api, uint8_t *destination_address, uint16_t phy_mtu);

/**
 * @brief Write synchronization info to given pointer.
 * @param api FHSS instance.
 * @param ptr Pointer to data. Start of written data for Synch frame. Start of IE header for Data frame.
 * @param length Length of IE header. Ignored when Synch frame.
 * @param frame_type Frame type of packet (Frames types are defined by FHSS api).
 * @param tx_time TX time must be referenced to the first symbol following the SFD of the transmitted frame.
 * @return -1 on fail, write length otherwise.
 */
typedef int16_t fhss_write_synch_info(const fhss_api_t *api, uint8_t *ptr, uint8_t length, int frame_type, uint32_t tx_time);

/**
 * @brief Initialize MAC functions.
 * @param api FHSS instance.
 * @param callbacks MAC functions to be called from FHSS.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int fhss_init_callbacks(const fhss_api_t *api, fhss_callback_t *callbacks);

/**
 * @brief Result of fhss_lfn_tx_check (Wi-SUN FAN 1.1 LFN parent, GS8).
 */
typedef enum {
    FHSS_LFN_TX_NOT_LFN = 0,    /**< Not an LFN destination: the usual FHSS rules apply. */
    FHSS_LFN_TX_NOW,            /**< LFN destination: send now, starting at *tx_time (its next listen window). */
    FHSS_LFN_TX_WAIT,           /**< LFN destination: hold the frame; the FHSS polls the TX queue before the window. */
    FHSS_LFN_TX_EXPIRED         /**< LFN destination: the frame waited longer than its TTL; drop it as timed out. */
} fhss_lfn_tx_e;

/**
 * @brief Check whether a unicast frame goes to an LFN, and when it may be sent.
 *
 * Called by the MAC for every acknowledged unicast frame with a 64-bit
 * destination before it is sent or taken from the queue. Optional: may be
 * NULL. Returns FHSS_LFN_TX_NOT_LFN unless LFN parenting is enabled in the
 * FHSS and the destination has an LFN unicast schedule.
 * @param api FHSS instance.
 * @param destination_address Destination MAC address (64-bit).
 * @param queued_since_us Timestamp of the frame's first request (its TTL starts there).
 * @param tx_time Written with FHSS_LFN_TX_NOW: absolute timestamp (µs) to start the transmission.
 * @return See fhss_lfn_tx_e.
 */
typedef fhss_lfn_tx_e fhss_lfn_tx_check(const fhss_api_t *api, const uint8_t *destination_address, uint32_t queued_since_us, uint32_t *tx_time);

/**
 * @brief A frame to an LFN at a requested time (an LPA in an LND discovery
 * slot, GS8 stage 5): NOW when the time is less than the parent's lead away
 * (*tx_time: the requested time, or the earliest possible), WAIT before
 * (the FHSS polls the queue), EXPIRED once it has passed by more than
 * 20 ms; NOT_LFN if the destination has no LFN schedule. Optional.
 */
typedef fhss_lfn_tx_e fhss_lfn_tx_at(const fhss_api_t *api, const uint8_t *destination_address, uint32_t requested_us, uint32_t *tx_time);

/**
 * \brief Struct fhss_api defines interface between software MAC and FHSS.
 * Application creates fhss_api_s object by calling FHSS creator function.
 * Then object is passed to software MAC which then initialises it's own callback functions.
 */
struct fhss_api {
    fhss_is_broadcast_channel *is_broadcast_channel;    /**< FHSS is broadcast channel. */
    fhss_use_broadcast_queue *use_broadcast_queue;      /**< FHSS queue check. */
    fhss_tx_handle *tx_handle;                          /**< FHSS TX handle. */
    fhss_check_tx_conditions *check_tx_conditions;      /**< Check TX permission. */
    fhss_receive_frame *receive_frame;                  /**< Notification of received FHSS synch or synch request frame. */
    fhss_data_tx_done *data_tx_done;                    /**< Data TX done callback. */
    fhss_data_tx_fail *data_tx_fail;                    /**< Data TX or CCA failed callback. */
    fhss_synch_state_set *synch_state_set;              /**< Change synchronization state. */
    fhss_read_timestamp *read_timestamp;                /**< Read timestamp. */
    fhss_get_retry_period *get_retry_period;            /**< Get retransmission period. */
    fhss_write_synch_info *write_synch_info;            /**< Write synchronization info to TX frame*/
    fhss_init_callbacks *init_callbacks;                /**< Initialize MAC functions. */
    fhss_lfn_tx_check *lfn_tx_check;                    /**< LFN destination check (FAN 1.1 parent). Optional, may be NULL. */
    fhss_lfn_tx_at *lfn_tx_at;                          /**< Frame to an LFN at a requested time (FAN 1.1 parent). Optional, may be NULL. */
};

/**
 * @brief Read MAC TX queue size.
 * @param fhss_api FHSS instance.
 * @param broadcast_queue Queue type to be read (true if broadcast queue).
 * @return Queue size (number of frames queued).
 */
typedef uint16_t mac_read_tx_queue_size(const fhss_api_t *fhss_api, bool broadcast_queue);

/**
 * @brief Read MAC address.
 * @param fhss_api FHSS instance.
 * @param mac_address MAC address pointer.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int mac_read_mac_address(const fhss_api_t *fhss_api, uint8_t *mac_address);

/**
 * @brief Read PHY datarate.
 * @param fhss_api FHSS instance.
 * @return PHY datarate.
 */
typedef uint32_t mac_read_datarate(const fhss_api_t *fhss_api);

/**
 * @brief Read 32-bit timestamp.
 * @param fhss_api FHSS instance.
 * @return Timestamp.
 */
typedef uint32_t mac_read_timestamp(const fhss_api_t *fhss_api);

/**
 * @brief Change channel.
 * @param fhss_api FHSS instance.
 * @param channel_number Channel number.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int mac_change_channel(const fhss_api_t *fhss_api, uint8_t channel_number);

/**
 * @brief Poll TX queue.
 * @param fhss_api FHSS instance.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int mac_tx_poll(const fhss_api_t *fhss_api);

/**
 * @brief Broadcast channel notification from FHSS.
 * @param fhss_api FHSS instance.
 * @param broadcast_time Remaining broadcast time.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int mac_broadcast_notify(const fhss_api_t *fhss_api, uint32_t broadcast_time);

/**
 * @brief Read coordinator MAC address.
 * @param fhss_api FHSS instance.
 * @param mac_address MAC address pointer.
 * @return  0 Success.
 * @return -1 Invalid parameters.
 */
typedef int mac_read_coordinator_mac_address(const fhss_api_t *fhss_api, uint8_t *mac_address);

/**
 * @brief Read synchronization info for a specific peer.
 * @param fhss_api FHSS instance.
 * @param info_ptr Pointer to info data.
 * @param mac_address MAC address pointer.
 * @param info_type Type of the read info (UTT-IE, BT-IE, US-IE, BS-IE).
 * @param rx_timestamp Time of reception of the element.
 * @return  >=0 Length of the info.
 * @return -1 Fail.
 */
typedef int mac_read_synch_info(const fhss_api_t *fhss_api, uint8_t *info_ptr, uint8_t *mac_address, int info_type, uint32_t rx_timestamp);

/**
 * @brief Events of the timed receive windows (Wi-SUN FAN 1.1 LFN, GS8).
 */
typedef enum {
    FHSS_RX_WINDOWS_START,  /**< The FHSS runs an LFN listen schedule: the receiver is on only in windows. */
    FHSS_RX_WINDOWS_STOP,   /**< Back to the usual behaviour (macRxOnWhenIdle decides). */
    FHSS_RX_WINDOW_OPEN,    /**< Open a receive window on the given channel now. */
    FHSS_RX_WINDOW_CLOSE    /**< Close the open window: radio off unless the MAC is busy. */
} fhss_rx_window_e;

/**
 * @brief Timed receive window: the FHSS opens and closes it at the times of
 * its listen schedule (FHSS timers), the MAC turns the radio on and off.
 * @param fhss_api FHSS instance.
 * @param event See fhss_rx_window_e.
 * @param channel Channel of the window (FHSS_RX_WINDOW_OPEN only).
 * @return 0 Done.
 * @return 1 FHSS_RX_WINDOW_CLOSE only: the radio is receiving or sending an
 *           ACK, so it stays on; the FHSS asks again a little later.
 * @return -1 Fail.
 */
typedef int mac_rx_window(const fhss_api_t *fhss_api, fhss_rx_window_e event, uint8_t channel);

/**
 * \brief Struct fhss_callback defines functions that software MAC needs to implement.
 * Function pointers are passed to FHSS using fhss_init_callbacks function.
 */
struct fhss_callback {
    mac_read_tx_queue_size *read_tx_queue_size;                 /**< Read MAC TX queue size. */
    mac_read_mac_address *read_mac_address;                     /**< Read MAC address. */
    mac_read_datarate *read_datarate;                           /**< Read PHY datarate. */
    mac_read_timestamp *read_timestamp;                         /**< Read timestamp. */
    mac_change_channel *change_channel;                         /**< Change channel. */
    mac_tx_poll *tx_poll;                                       /**< Poll TX queue. */
    mac_broadcast_notify *broadcast_notify;                     /**< Broadcast channel notification from FHSS. */
    mac_read_coordinator_mac_address *read_coord_mac_address;   /**< Read coordinator MAC address. */
    mac_read_synch_info *read_synch_info;                       /**< Read information element for a specific MAC address. */
    mac_rx_window *rx_window;                                   /**< Timed receive windows (FAN 1.1 LFN). May be NULL. */
};

#endif // FHSS_API_H
