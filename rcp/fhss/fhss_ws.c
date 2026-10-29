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
#include <stdint.h>
#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_config.h"
#include "fhss.h"
#include "fhss_common.h"
#include "fhss_statistics.h"
#include "channel_list.h"
#include "channel_functions.h"
#include "fhss_ws.h"
#include <stdlib.h>
#include "mbed-client-libservice/common_functions.h"
#include "nanostack-event-loop/eventOS_callback_timer.h"
#include "ns_rand.h"
#include "mbed-client-libservice/ns_trace.h"
#include "ns_port/critical.h"
#include <string.h>

#define TRACE_GROUP "fhss"

// Enable this flag to use channel traces
// #define FHSS_CHANNEL_DEBUG
// Enable this flag to use debug callbacks
// #define FHSS_CHANNEL_DEBUG_CBS

#ifdef FHSS_CHANNEL_DEBUG_CBS
void (*fhss_uc_switch)(void) = NULL;
void (*fhss_bc_switch)(void) = NULL;
#endif /*FHSS_CHANNEL_DEBUG_CBS*/
// Seconds to milliseconds
#define S_TO_MS(x) (((int64_t)x)*1000)
// Milliseconds to seconds
#define MS_TO_S(x) divide_integer(x, 1000)
// Seconds to microseconds
#define S_TO_US(x) (((int64_t)x)*1000000)
// Microseconds to seconds
#define US_TO_S(x) divide_integer(x, 1000000)
// Milliseconds to microseconds
#define MS_TO_US(x) (((int64_t)x)*1000)
// Microseconds to milliseconds
#define US_TO_MS(x) divide_integer(x, 1000)
// Milliseconds to nanoseconds
#define MS_TO_NS(x) (((int64_t)x)*1000000)
// Nanoseconds to milliseconds
#define NS_TO_MS(x) divide_integer(x, 1000000)
// Microseconds to nanoseconds
#define US_TO_NS(x) (((int64_t)x)*1000)
// Nanoseconds to microseconds
#define NS_TO_US(x) divide_integer(x, 1000)
#define DEF_2E24 0x1000000
#define IE_HEADER_LENGTH_MASK 0x007f
#define IE_HEADER_ID_MASK 0x7f80
#define WH_IE_ID        0x2a
#define WH_SUB_ID_UTT   1
#define WH_SUB_ID_BT    2
#define WH_SUB_ID_LUTT  10
#define WH_SUB_ID_LBT   11
#define WH_SUB_ID_LTO   14


struct ws_ie_t {
    uint8_t *content_ptr;
    unsigned length: 7;
    uint8_t id;
};

bool fhss_synch_global_state = false;
static int fhss_ws_manage_channel_table_allocation(fhss_structure_t *fhss_structure, uint16_t channel_count);
static void fhss_event_timer_cb(int timer_id, uint16_t slots);
static void fhss_ws_update_uc_channel_callback(fhss_structure_t *fhss_structure);
static void fhss_unicast_handler(const fhss_api_t *fhss_api, uint16_t delay);
static void fhss_broadcast_handler(const fhss_api_t *fhss_api, uint16_t delay);
static bool fhss_ws_check_tx_allowed(fhss_structure_t *fhss_structure);
static bool fhss_allow_transmitting_on_rx_slot(fhss_structure_t *fhss_structure);
static bool fhss_allow_unicast_on_broadcast_channel(fhss_structure_t *fhss_structure);
static int32_t fhss_channel_index_from_mask(const uint32_t *channel_mask, int32_t channel_index, uint16_t number_of_channels);
static void fhss_ws_lfn_timer_handler(const fhss_api_t *fhss_api, uint16_t delay);
static void fhss_ws_lfn_bc_timer_handler(const fhss_api_t *fhss_api, uint16_t delay);
static uint32_t fhss_ws_lfn_widening(struct fhss_ws_lfn *lfn, uint32_t t);
static uint32_t fhss_ws_lfn_local_us(const struct fhss_ws_lfn *lfn, uint32_t parent_us);
static int32_t fhss_ws_lfn_bc_channel(fhss_structure_t *fhss_structure, uint16_t slot, uint16_t bsi, uint8_t function, uint16_t fixed);
static bool fhss_ws_lfn_bc_sched(fhss_structure_t *fhss_structure, uint32_t now);
static void fhss_ws_lfn_write_lbt_lto(fhss_structure_t *fhss_structure, uint8_t *ptr, uint8_t length, uint32_t tx_time);
static void fhss_ws_lfn_listen_start(fhss_structure_t *fhss_structure);
static void fhss_ws_lfn_listen_stop(fhss_structure_t *fhss_structure);
static fhss_ws_neighbor_timing_info_t *fhss_ws_lfn_neighbor(fhss_structure_t *fhss_structure, const uint8_t *eui64);
static bool fhss_ws_lfn_neighbor_sched(fhss_structure_t *fhss_structure, fhss_ws_neighbor_timing_info_t *info, fhss_lfn_sched_t *sched);
static int32_t fhss_ws_lfn_neighbor_channel(fhss_structure_t *fhss_structure, fhss_ws_neighbor_timing_info_t *info, uint16_t slot, uint8_t *eui64);
static fhss_lfn_tx_e fhss_ws_lfn_tx_check_callback(const fhss_api_t *api, const uint8_t *destination_address, uint32_t queued_since_us, uint32_t *tx_time);
static bool fhss_ws_lfn_uplink_blocked(fhss_structure_t *fhss_structure, uint32_t t, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length);

// This function supports rounding up
static int64_t divide_integer(int64_t dividend, int32_t divisor)
{
    if (!divisor) {
        return dividend;
    }
    if (dividend < 0) {
        return (dividend - divisor / 2) / divisor;
    }
    return (dividend + divisor / 2) / divisor;
}

static uint32_t get_remaining_slots_us(fhss_structure_t *fhss_structure, void (*callback)(const fhss_api_t *api, uint16_t), uint32_t max_timeout_us)
{
    uint32_t remaining_time_us = fhss_structure->platform_functions.fhss_get_remaining_slots(callback, fhss_structure->fhss_api);
    // When remaining time goes negative, use 0.
    if (remaining_time_us > max_timeout_us) {
        remaining_time_us = 0;
    }
    return remaining_time_us;
}

void fhss_ws_start_timer(fhss_structure_t *fhss_structure, uint32_t time, void (*callback)(const fhss_api_t *fhss_api, uint16_t))
{
    // Number of millisecond slots in timeout(us)
    int32_t time_in_ms = divide_integer(time, 1000);
    // Reduce the compensation (per millisecond) from timeout.
    time -= NS_TO_US(time_in_ms * fhss_structure->ws->drift_per_millisecond_ns);
    fhss_start_timer(fhss_structure, time, callback);
    if (callback == fhss_unicast_handler) {
        fhss_structure->ws->next_uc_timeout = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) + time;
    } else if (callback == fhss_broadcast_handler) {
        fhss_structure->ws->next_bc_timeout = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) + time;
    }
}

fhss_structure_t *fhss_ws_enable(fhss_api_t *fhss_api, const fhss_ws_configuration_t *fhss_configuration, const fhss_timer_t *fhss_timer)
{
    if (!fhss_api || !fhss_configuration || !fhss_timer) {
        tr_err("Invalid FHSS enable configuration");
        return NULL;
    }
    int bc_channel_count = channel_list_count_channels(fhss_configuration->channel_mask);
    int uc_channel_count = channel_list_count_channels(fhss_configuration->unicast_channel_mask);

    if (bc_channel_count <= 0) {
        // There must be at least one configured channel in channel list
        return NULL;
    }
    fhss_structure_t *fhss_struct = fhss_allocate_instance(fhss_api, fhss_timer);
    if (!fhss_struct) {
        return NULL;
    }
    fhss_struct->ws = malloc(sizeof(fhss_ws_t));
    if (!fhss_struct->ws) {
        fhss_free_instance(fhss_api);
        return NULL;
    }
    memset(fhss_struct->ws, 0, sizeof(fhss_ws_t));
    if (fhss_ws_manage_channel_table_allocation(fhss_struct, uc_channel_count > bc_channel_count ? uc_channel_count : bc_channel_count)) {
        free(fhss_struct->ws);
        fhss_free_instance(fhss_api);
        tr_error("Failed to allocate channel tables");
        return NULL;
    }

    fhss_struct->fhss_event_timer = eventOS_callback_timer_register(fhss_event_timer_cb);
    fhss_struct->ws->fhss_configuration = *fhss_configuration;
    if (uc_channel_count == 0) {
        //If Unicast channel is empty use Domain mask
        for (uint8_t i = 0; i < 8; i++) {
            fhss_struct->ws->fhss_configuration.unicast_channel_mask[i] = fhss_configuration->channel_mask[i];
        }
        uc_channel_count = bc_channel_count;
    }

    fhss_struct->number_of_channels = fhss_configuration->channel_mask_size;
    fhss_struct->number_of_uc_channels = uc_channel_count;
    fhss_struct->number_of_bc_channels = bc_channel_count;
    fhss_struct->optimal_packet_length = OPTIMAL_PACKET_LENGTH;
    fhss_ws_set_hop_count(fhss_struct, 0xff);
    fhss_struct->rx_channel = fhss_configuration->unicast_fixed_channel;
    fhss_struct->ws->min_synch_interval = DEFAULT_MIN_SYNCH_INTERVAL;
    // By default, allow transmitting unicast data only on TX slots.
    fhss_struct->ws->tx_level = WS_TX_SLOT;
    // By default, allow always transmitting unicast data in expedited forwarding mode.
    fhss_struct->ws->ef_tx_level = WS_TX_ALWAYS;
    ns_list_init(&fhss_struct->fhss_failed_tx_list);
    return fhss_struct;
}

static int fhss_ws_manage_channel_table_allocation(fhss_structure_t *fhss_structure, uint16_t channel_count)
{
    // Must allocate channel table for TR51
    if (!fhss_structure->ws->tr51_channel_table && !fhss_structure->ws->tr51_output_table) {
        fhss_structure->ws->tr51_channel_table = malloc(sizeof(int16_t) * tr51_calc_nearest_prime_number(channel_count));
        if (!fhss_structure->ws->tr51_channel_table) {
            return -1;
        }
        fhss_structure->ws->tr51_output_table = malloc(sizeof(int16_t) * channel_count);
        if (!fhss_structure->ws->tr51_output_table) {
            free(fhss_structure->ws->tr51_channel_table);
            return -1;
        }
        tr51_init_channel_table(fhss_structure->ws->tr51_channel_table, channel_count);
    }
    return 0;
}

void fhss_set_txrx_slot_length(fhss_structure_t *fhss_structure)
{
    // No broadcast schedule, no TX slots
    if (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval == 0 || fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval == 0) {
        return;
    }
    uint32_t txrx_slot_length_ms_tmp = WS_TXRX_SLOT_LEN_MS_MAX;
    if (fhss_structure->callbacks.read_datarate) {
        /* Calculate minimum TX slot length which can fit optimal packet length twice.
         * Twice, because 0, 1, 4, 5... hop starts transmission at the beginning of TX slot and 2, 3, 6, 7... hop at the middle of TX slot
         *
         * hop 0
         * tx'ing            |    |            | |               |   |
         * |   BC   |   RX   |   TX   |   RX   |   TX   |   RX   |   TX   |   BC   |
         * hop 1
         * tx'ing   |    |            | |               |   |
         * |   BC   |   TX   |   RX   |   TX   |   RX   |   TX   |   RX   |   BC   |
         * hop 2
         * tx'ing                 |   |             | |               |   |
         * |   BC   |   RX   |   TX   |   RX   |   TX   |   RX   |   TX   |   BC   |
         * hop 3
         * tx'ing        |   |             | |               |   |
         * |   BC   |   TX   |   RX   |   TX   |   RX   |   TX   |   RX   |   BC   |
         */
        uint32_t datarate = fhss_structure->callbacks.read_datarate(fhss_structure->fhss_api);
        if (datarate) {
            txrx_slot_length_ms_tmp = ((fhss_structure->optimal_packet_length * 2) * (8000000 / datarate)) / 1000;
            // Do not allow using too high TX slot length.
            if (txrx_slot_length_ms_tmp > WS_TXRX_SLOT_LEN_MS_MAX) {
                tr_debug("TX slot length setting too high %"PRIu32"ms, using %"PRIu32"ms", txrx_slot_length_ms_tmp, (uint32_t)WS_TXRX_SLOT_LEN_MS_MAX);
                txrx_slot_length_ms_tmp = WS_TXRX_SLOT_LEN_MS_MAX;
            }
            // Do not allow using too low TX slot length.
            if (txrx_slot_length_ms_tmp < WS_TXRX_SLOT_LEN_MS_MIN) {
                tr_debug("TX slot length setting too low %"PRIu32"ms, using %"PRIu32"ms", txrx_slot_length_ms_tmp, (uint32_t)WS_TXRX_SLOT_LEN_MS_MIN);
                txrx_slot_length_ms_tmp = WS_TXRX_SLOT_LEN_MS_MIN;
            }
        }
    }
    uint32_t number_of_tx_slots = ((fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) / txrx_slot_length_ms_tmp) / 2;
    if (!number_of_tx_slots) {
        return;
    }
    fhss_structure->ws->txrx_slot_length_ms = (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) / (number_of_tx_slots * 2);
    tr_info("TX slot length: %"PRIu32"ms", fhss_structure->ws->txrx_slot_length_ms);
}

static uint32_t fhss_get_number_of_tx_slots(fhss_structure_t *fhss_structure)
{
    return ((fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) / fhss_structure->ws->txrx_slot_length_ms) / 2;
}

static int32_t fhss_ws_calc_bc_channel(fhss_structure_t *fhss_structure)
{
    int32_t next_channel = fhss_structure->ws->fhss_configuration.broadcast_fixed_channel;

    if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_TR51CF) {
        next_channel = tr51_get_bc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table, fhss_structure->ws->bc_slot, fhss_structure->ws->fhss_configuration.bsi, fhss_structure->number_of_bc_channels, NULL);
        next_channel = fhss_channel_index_from_mask(fhss_structure->ws->fhss_configuration.channel_mask, next_channel, fhss_structure->number_of_channels);
        if (++fhss_structure->ws->bc_slot == fhss_structure->number_of_bc_channels) {
            fhss_structure->ws->bc_slot = 0;
        }
    } else if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_DH1CF) {
        fhss_structure->ws->bc_slot++;
        next_channel = dh1cf_get_bc_channel_index(fhss_structure->ws->bc_slot, fhss_structure->ws->fhss_configuration.bsi, fhss_structure->number_of_bc_channels);
        next_channel = fhss_channel_index_from_mask(fhss_structure->ws->fhss_configuration.channel_mask, next_channel, fhss_structure->number_of_channels);
    } else if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_VENDOR_DEF_CF) {
        if (fhss_structure->ws->fhss_configuration.vendor_defined_cf) {
            next_channel = fhss_structure->ws->fhss_configuration.vendor_defined_cf(fhss_structure->fhss_api, fhss_structure->ws->bc_slot, NULL, fhss_structure->ws->fhss_configuration.bsi, fhss_structure->number_of_channels);
        }
    }
#ifdef FHSS_CHANNEL_DEBUG
    tr_info("%"PRIu32" BC %u %u %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), next_channel, fhss_structure->ws->bc_slot, fhss_structure->ws->fhss_configuration.bsi);
#endif /*FHSS_CHANNEL_DEBUG*/
    return next_channel;
}

static uint8_t calc_own_tx_trig_slot(uint8_t own_hop)
{
    if (own_hop == 0xff) {
        return 0;
    }
    if (own_hop & 1) {
        own_hop--;
    }
    own_hop /= 2;
    return (own_hop & 1);
}

static int32_t fhss_channel_index_from_mask(const uint32_t *channel_mask, int32_t channel_index, uint16_t number_of_channels)
{
    //Function will return real active channel index at list
    int32_t active_channels = 0;
    // Set channel maks outside excluded channels
    for (int32_t i = 0; i < number_of_channels; i++) {
        if (channel_mask[i / 32] & (1U << (i % 32))) {
            if (channel_index == active_channels) {
                return i;
            }
            active_channels++;
        }
    }
    return 0;
}

static void fhss_broadcast_handler(const fhss_api_t *fhss_api, uint16_t delay)
{
    (void) delay;
    int32_t next_channel;
    // The trace and the TX queue poll timer are done after the critical
    // section, from values captured inside it. The section masks interrupts
    // on targets; a trace and a random draw (which may refill a TRNG pool)
    // inside it made it last up to ~600 us on the EFR32FG25, long enough for
    // the radio to drop a scheduled transmission.
    uint32_t trace_timestamp;
    uint16_t trace_bc_slot;
    bool trace_on_bc_channel;
    bool poll_random = false;
    uint16_t poll_min_random = 0, poll_max_random = 0;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure) {
        return;
    }
    // LFN parent: keep the LFN broadcast schedule's anchor near (32-bit clock)
    fhss_ws_lfn_bc_sched(fhss_structure, fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api));
    platform_enter_critical();
    if (fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval == 0 || fhss_structure->ws->fhss_configuration.fhss_broadcast_interval == 0) {
        // stop broadcast schedule
        fhss_structure->ws->is_on_bc_channel = false;
        fhss_structure->ws->synchronization_time = 0;
        fhss_structure->ws->broadcast_timer_running = false;
        platform_exit_critical();
        return;
    }

    int32_t delay_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) - fhss_structure->ws->next_bc_timeout;
    // Assume this was initial call for this function without timer
    if (!fhss_structure->ws->bc_slot && !fhss_structure->ws->next_bc_timeout) {
        delay_us = 0;
    }

    if (fhss_structure->ws->is_on_bc_channel == false) {
        // If callback was delayed longer than broadcast interval, BC slot must be updated
        fhss_structure->ws->bc_slot += (US_TO_MS(delay_us) + (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval)) / fhss_structure->ws->fhss_configuration.fhss_broadcast_interval;
        // We weren't on broadcast channel. Check if after delayed interrupt we still shouldn't go to broadcast channel. Setting is_on_bc_channel to true forces to skip BC dwell interval next.
        if ((US_TO_MS(delay_us) % fhss_structure->ws->fhss_configuration.fhss_broadcast_interval) > fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) {
            fhss_structure->ws->is_on_bc_channel = true;
            // If delay + assumed timeout is longer than broadcast interval, delay value must be computed differently to make compensation work properly
            if (US_TO_MS(delay_us) + (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) > fhss_structure->ws->fhss_configuration.fhss_broadcast_interval) {
                delay_us = (MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) + delay_us) % MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval);
            }
        } else {
            if (US_TO_MS(delay_us) + (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) > fhss_structure->ws->fhss_configuration.fhss_broadcast_interval) {
                delay_us = ((MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) + delay_us) % MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval)) - MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval);
            }
        }
    } else {
        // If callback was delayed longer than broadcast interval, BC slot must be updated
        fhss_structure->ws->bc_slot += (US_TO_MS(delay_us) + fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) / fhss_structure->ws->fhss_configuration.fhss_broadcast_interval;
        // We were on broadcast channel. Check if after delayed interrupt we should go broadcast channel again. Setting is_on_bc_channel to false forces to start BC dwell interval next.
        if ((US_TO_MS(delay_us) % fhss_structure->ws->fhss_configuration.fhss_broadcast_interval) > (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval)) {
            fhss_structure->ws->is_on_bc_channel = false;
        }
        delay_us %= MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval);
        delay_us %= MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval);
    }

    if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_TR51CF) {
        fhss_structure->ws->bc_slot %= fhss_structure->number_of_bc_channels;
    }

    trace_timestamp = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    trace_bc_slot = fhss_structure->ws->bc_slot;
    trace_on_bc_channel = fhss_structure->ws->is_on_bc_channel;
    if (fhss_structure->ws->is_on_bc_channel == false) {
        fhss_ws_start_timer(fhss_structure, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) - ((int64_t) delay_us * fhss_structure->platform_functions.fhss_resolution_divider), fhss_broadcast_handler);
        fhss_structure->ws->is_on_bc_channel = true;
        next_channel = fhss_structure->ws->bc_channel = fhss_ws_calc_bc_channel(fhss_structure);
        if (fhss_structure->ws->expedited_forwarding_enabled_us) {
            eventOS_callback_timer_start(fhss_structure->fhss_event_timer, EXPEDITED_FORWARDING_POLL_PERIOD);
        } else {
            /* Start timer with random timeout to trigger broadcast TX queue poll event.
             * Min random is 1/50 of the channel dwell interval.
             * Max random is 3/4 of the channel dwell interval.
             * Event timer resolution is 50us.
             */
            uint32_t bc_dwell_us = MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval);
            poll_min_random = (bc_dwell_us / 50) / 50;
            poll_max_random = (bc_dwell_us - (bc_dwell_us / 4)) / 50;
            poll_random = true;
        }
    } else {
        fhss_structure->ws->unicast_start_time_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
        uint32_t timeout = MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval);
        fhss_ws_start_timer(fhss_structure, timeout - (delay_us * fhss_structure->platform_functions.fhss_resolution_divider), fhss_broadcast_handler);
        fhss_structure->ws->is_on_bc_channel = false;
        // Should return to own (unicast) listening channel after broadcast channel
        next_channel = fhss_structure->rx_channel;
        if (fhss_structure->ws->expedited_forwarding_enabled_us) {
            eventOS_callback_timer_start(fhss_structure->fhss_event_timer, EXPEDITED_FORWARDING_POLL_PERIOD);
        } else {
            /* Start timer with random timeout to trigger unicast TX queue poll event.
             * For hops 0,1,4,5,8,9,...
             * Min random is 1/100 of the TX slot length.
             * Max random is 1/5 of the TX slot length.
             *
             * For hops 2,3,6,7,10,11,...
             * Min random is 1/100 of the TX slot length plus 0.5*TX slot length.
             * Max random is 1/10 of the TX slot length plus 0.5*TX slot length.
             * Event timer resolution is 50us.
             */
            // returns 1 if polling of TX queue is done on latter half of the TX slot
            uint8_t own_tx_trig_slot = calc_own_tx_trig_slot(fhss_structure->own_hop);
            uint32_t txrx_slot_length_us = MS_TO_US(fhss_structure->ws->txrx_slot_length_ms);
            uint16_t uc_min_random = (((txrx_slot_length_us / 2) * own_tx_trig_slot) / 50) + ((txrx_slot_length_us / 100) / 50);
            uint16_t uc_max_random = (((txrx_slot_length_us / 2) * own_tx_trig_slot) / 50) + ((txrx_slot_length_us / 5) / 50);
            bool tx_allowed = fhss_ws_check_tx_allowed(fhss_structure);
            if (!tx_allowed) {
                uc_min_random += (txrx_slot_length_us) / 50;
                uc_max_random += (txrx_slot_length_us) / 50;
            }
            poll_min_random = uc_min_random;
            poll_max_random = uc_max_random;
            poll_random = true;
        }

#ifdef FHSS_CHANNEL_DEBUG
        tr_info("%"PRIu32" UC %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), fhss_structure->rx_channel);
#endif /*FHSS_CHANNEL_DEBUG*/
#ifdef TIMING_TOOL_TRACES
        tr_info("%u UC_change %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), next_channel);
#endif
    }
    fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, next_channel);
#ifdef FHSS_CHANNEL_DEBUG_CBS
    if (fhss_bc_switch) {
        fhss_bc_switch();
    }
#endif /*FHSS_CHANNEL_DEBUG_CBS*/
#ifdef TIMING_TOOL_TRACES
    if (fhss_structure->ws->is_on_bc_channel == true) {
        tr_info("%u BC_start %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), next_channel);
    } else {
        tr_info("%u BC_done", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api));
    }
#endif
    platform_exit_critical();
    tr_info("%"PRIu32" BC slot: %d On BC ch %d", trace_timestamp, trace_bc_slot, trace_on_bc_channel);
    if (poll_random) {
        eventOS_callback_timer_start(fhss_structure->fhss_event_timer, rand_get_random_in_range(poll_min_random, poll_max_random));
    }
}

static int own_floor(float value)
{
    return (int)value;
}

static int own_ceil(float value)
{
    int ivalue = (int)value;
    if (value == (float)ivalue) {
        return ivalue;
    }
    return ivalue + 1;
}

static void fhss_event_timer_cb(int timer_id, uint16_t slots)
{
    (void) slots;
    uint16_t queue_size = 0;

    fhss_structure_t *fhss_structure = fhss_get_object_with_timer_id(timer_id);
    if (!fhss_structure) {
        return;
    }

    if (fhss_structure->ws->lfn.listen_running) {
        // LFN: no periodic poll (it would wake the node every 10 ms); a
        // frame queued behind another is sent from the MAC's TX confirm.
        // This timer polls after a failed frame (it may wait for a restart)
        // or the parent's broadcast dwell, and again while the queue holds
        // frames.
        if (fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false)) {
            fhss_structure->callbacks.tx_poll(fhss_structure->fhss_api);
            if (fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false)) {
                eventOS_callback_timer_start(fhss_structure->fhss_event_timer, FHSS_LFN_QUEUE_POLL_US / 50);
            }
        }
        return;
    }

    if (fhss_structure->ws->expedited_forwarding_enabled_us) {
        if ((fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) - fhss_structure->ws->expedited_forwarding_enabled_us) > S_TO_US(EXPEDITED_FORWARDING_PERIOD)) {
            fhss_structure->ws->expedited_forwarding_enabled_us = 0;
        }
        eventOS_callback_timer_start(fhss_structure->fhss_event_timer, EXPEDITED_FORWARDING_POLL_PERIOD);
        if (fhss_structure->ws->is_on_bc_channel == true) {
            queue_size = fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, true);
        } else {
            queue_size = fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false);
        }
    } else if ((fhss_structure->ws->unicast_timer_running == false) && (fhss_structure->ws->broadcast_timer_running == false)) {
        // No one would poll TX queue if schedule timers were not started. Start poll timer with default interval.
        eventOS_callback_timer_start(fhss_structure->fhss_event_timer, DEFAULT_POLL_PERIOD);
        queue_size = fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false);
    } else {
        if (fhss_structure->ws->is_on_bc_channel == true) {
            queue_size = fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, true);
        } else {
            // On unicast, start timer to trigger polling event on next TX slot
            uint32_t delay_between_tx_slots_us = MS_TO_US(fhss_structure->ws->txrx_slot_length_ms) * 2;
            // Timer could drift to RX slot when broadcast interval is high. Return timer to TX slot.
            if (fhss_ws_check_tx_allowed(fhss_structure) == false) {
                delay_between_tx_slots_us -= MS_TO_US(fhss_structure->ws->txrx_slot_length_ms - (calc_own_tx_trig_slot(fhss_structure->own_hop) * (fhss_structure->ws->txrx_slot_length_ms / 2)));
            }
            if (delay_between_tx_slots_us < get_remaining_slots_us(fhss_structure, fhss_broadcast_handler, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval))) {
                eventOS_callback_timer_start(fhss_structure->fhss_event_timer, delay_between_tx_slots_us / 50);
            }
            queue_size = fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false);
        }
    }
#ifdef TIMING_TOOL_TRACES
    tr_info("On BC ch %d , %sC q size %d", fhss_structure->ws->is_on_bc_channel == true ? 1 : 0, fhss_structure->ws->is_on_bc_channel == true ? "B" : "U" ,queue_size);
#endif
    if (queue_size) {
        fhss_structure->callbacks.tx_poll(fhss_structure->fhss_api);
#ifdef TIMING_TOOL_TRACES
        tr_info("%u TX_poll", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api));
#endif
    }
}

static uint32_t fhss_ws_calculate_ufsi(fhss_structure_t *fhss_structure, uint32_t tx_time)
{
    uint8_t dwell_time = fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval;
    uint16_t cur_slot = fhss_structure->ws->uc_slot;
    if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_TR51CF) {
        if (cur_slot == 0) {
            cur_slot = fhss_structure->number_of_uc_channels;
        }
    }
    cur_slot--;

    uint32_t time_to_tx = 0;
    uint32_t cur_time = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    // High time to TX value (1000ms) is because actual TX time already passed.
    if (US_TO_MS(tx_time - cur_time) < 1000) {
        time_to_tx = US_TO_MS(tx_time - cur_time);
    }
    uint64_t ms_since_seq_start;
    if (fhss_structure->ws->unicast_timer_running == true) {
        // Allow timer interrupt to delay max 10 seconds, otherwise assume next_uc_timeout overflowed
        if ((fhss_structure->ws->next_uc_timeout < cur_time) && ((cur_time - fhss_structure->ws->next_uc_timeout) < 10000000)) {
            // The unicast timer has already expired, so count all previous slots
            // plus 1 completed slot
            // plus the time from timer expiration to now
            // plus the time until Tx
            ms_since_seq_start = ((cur_slot + 1) * dwell_time) + US_TO_MS(cur_time - fhss_structure->ws->next_uc_timeout) + time_to_tx;
        } else {
            // The unicast timer is still running, so count all previous slots
            // plus the remaining time in the slot
            // plus the time until Tx
            uint32_t remaining_time_ms  = US_TO_MS(fhss_structure->ws->next_uc_timeout - cur_time);
            ms_since_seq_start = (cur_slot * dwell_time) + (dwell_time - remaining_time_ms) + time_to_tx;
        }
    } else {
        // The unicast timer is not running. Act as if the slot has completed.
        // count all previous slots
        // plus 1 completed slot
        // plus the time until Tx
        ms_since_seq_start = ((cur_slot + 1) * dwell_time) + time_to_tx;
    }

    uint32_t seq_length = 0x10000;
    if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_TR51CF) {
        ms_since_seq_start %= (dwell_time * fhss_structure->number_of_uc_channels);
        seq_length = fhss_structure->number_of_uc_channels;
    }
    return own_floor((float)(ms_since_seq_start * DEF_2E24) / (seq_length * dwell_time));
}

static uint32_t fhss_ws_calculate_broadcast_interval_offset(fhss_structure_t *fhss_structure, uint32_t tx_time)
{
    uint8_t dwell_time = fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval;
    uint32_t broadcast_interval = fhss_structure->ws->fhss_configuration.fhss_broadcast_interval;
    uint32_t remaining_time_ms = US_TO_MS(get_remaining_slots_us(fhss_structure, fhss_broadcast_handler, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval)));
    if (fhss_structure->ws->is_on_bc_channel == true) {
        remaining_time_ms += (broadcast_interval - dwell_time);
    }
    uint32_t time_to_tx = 0;
    uint32_t cur_time = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    if (cur_time < tx_time) {
        time_to_tx = US_TO_MS(tx_time - cur_time);
    }
    return (broadcast_interval - remaining_time_ms) + time_to_tx;
}

static uint16_t fhss_ws_calculate_destination_slot(fhss_ws_neighbor_timing_info_t *neighbor_timing_info, uint32_t tx_time)
{
    uint_fast24_t ufsi = neighbor_timing_info->uc_timing_info.ufsi;
    uint32_t ufsi_timestamp = neighbor_timing_info->uc_timing_info.utt_rx_timestamp;
    uint8_t dwell_time = neighbor_timing_info->uc_timing_info.unicast_dwell_interval;
    uint32_t seq_length = 0x10000;
    if (neighbor_timing_info->uc_timing_info.unicast_channel_function  == WS_TR51CF) {
        seq_length = neighbor_timing_info->uc_timing_info.unicast_number_of_channels;
    }
    uint32_t dest_ms_since_seq_start = own_ceil((float)((uint64_t)ufsi * seq_length * dwell_time) / DEF_2E24);
    return (own_floor(((float)(US_TO_MS(tx_time - ufsi_timestamp) + dest_ms_since_seq_start) / dwell_time)) % seq_length);
}

static uint32_t fhss_ws_get_sf_timeout_callback(fhss_structure_t *fhss_structure)
{
    return MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval);
}

static int16_t fhss_ws_synch_state_set_callback(const fhss_api_t *api, fhss_states fhss_state, uint16_t pan_id)
{
    (void) pan_id;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return -1;
    }
    if (fhss_state == FHSS_EXPEDITED_FORWARDING) {
        if (!fhss_structure->ws->expedited_forwarding_enabled_us) {
            eventOS_callback_timer_start(fhss_structure->fhss_event_timer, EXPEDITED_FORWARDING_POLL_PERIOD);
        }
        fhss_structure->ws->expedited_forwarding_enabled_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
        if (!fhss_structure->ws->expedited_forwarding_enabled_us) {
            fhss_structure->ws->expedited_forwarding_enabled_us++;
        }
        return 0;
    }
    if (fhss_state == FHSS_SYNCHRONIZED && fhss_structure->ws->lfn.listen_enabled) {
        // LFN: the listen schedule instead of the FFN unicast and broadcast
        // schedules
        fhss_synch_global_state = true;
        fhss_structure->fhss_state = fhss_state;
        fhss_ws_lfn_listen_start(fhss_structure);
    } else if (fhss_state == FHSS_SYNCHRONIZED) {
        fhss_synch_global_state = true;
        uint32_t fhss_broadcast_interval = fhss_structure->ws->fhss_configuration.fhss_broadcast_interval;
        uint8_t fhss_bc_dwell_interval = fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval;
        fhss_set_txrx_slot_length(fhss_structure);
        // Start broadcast schedule when BC intervals are known
        if (fhss_broadcast_interval && fhss_bc_dwell_interval) {
            fhss_broadcast_handler(fhss_structure->fhss_api, 0);
            fhss_structure->ws->broadcast_timer_running = true;
        }
        // Start unicast schedule
        if ((fhss_structure->ws->fhss_configuration.ws_uc_channel_function != WS_FIXED_CHANNEL)) {
            fhss_ws_update_uc_channel_callback(fhss_structure);
            fhss_ws_start_timer(fhss_structure, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval), fhss_unicast_handler);
            fhss_structure->ws->unicast_timer_running = true;
        }
    } else if (fhss_state == FHSS_UNSYNCHRONIZED) {
        fhss_synch_global_state = false;
        if (fhss_structure->ws->lfn.listen_running) {
            fhss_ws_lfn_listen_stop(fhss_structure);
        }
        fhss_structure->ws->synchronization_time = 0;
        eventOS_callback_timer_stop(fhss_structure->fhss_event_timer);
        fhss_stop_timer(fhss_structure, fhss_unicast_handler);
        fhss_stop_timer(fhss_structure, fhss_broadcast_handler);
        fhss_structure->ws->broadcast_timer_running = false;
        fhss_structure->ws->is_on_bc_channel = false;
        fhss_structure->ws->synchronization_time = 0;
    }

    fhss_structure->fhss_state = fhss_state;
    int16_t current_channel = fhss_structure->rx_channel;
    if (fhss_structure->ws->is_on_bc_channel == true) {
        current_channel = fhss_structure->ws->bc_channel;
    }
    return current_channel;
}

static void fhss_ws_update_uc_channel_callback(fhss_structure_t *fhss_structure)
{
    uint8_t mac_address[8];
    int32_t next_channel = fhss_structure->rx_channel;
    fhss_structure->callbacks.read_mac_address(fhss_structure->fhss_api, mac_address);
    if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_FIXED_CHANNEL) {
        return;
    } else if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_TR51CF) {
        next_channel = tr51_get_uc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table, fhss_structure->ws->uc_slot, mac_address, fhss_structure->number_of_uc_channels, NULL);
        next_channel = fhss_structure->rx_channel = fhss_channel_index_from_mask(fhss_structure->ws->fhss_configuration.unicast_channel_mask, next_channel, fhss_structure->number_of_channels);
        if (++fhss_structure->ws->uc_slot == fhss_structure->number_of_uc_channels) {
            fhss_structure->ws->uc_slot = 0;
        }
    } else if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_DH1CF) {
        next_channel = dh1cf_get_uc_channel_index(fhss_structure->ws->uc_slot, mac_address, fhss_structure->number_of_uc_channels);
        next_channel = fhss_structure->rx_channel = fhss_channel_index_from_mask(fhss_structure->ws->fhss_configuration.unicast_channel_mask, next_channel, fhss_structure->number_of_channels);
        fhss_structure->ws->uc_slot++;
    } else if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_VENDOR_DEF_CF) {
        if (fhss_structure->ws->fhss_configuration.vendor_defined_cf) {
            next_channel = fhss_structure->rx_channel = fhss_structure->ws->fhss_configuration.vendor_defined_cf(fhss_structure->fhss_api, fhss_structure->ws->bc_slot, mac_address, fhss_structure->ws->fhss_configuration.bsi, fhss_structure->number_of_uc_channels);
        }
    }
    // Do not switch unicast channel when broadcast channel is active.
    if (fhss_structure->ws->is_on_bc_channel == true) {
        return;
    }
#ifdef FHSS_CHANNEL_DEBUG
    tr_info("%"PRIu32" UC %u %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), next_channel, fhss_structure->ws->uc_slot);
#endif /*FHSS_CHANNEL_DEBUG*/
#ifdef TIMING_TOOL_TRACES
    tr_info("%u UC_change %u", fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api), next_channel);
#endif
    fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, next_channel);
#ifdef FHSS_CHANNEL_DEBUG_CBS
    if (fhss_uc_switch) {
        fhss_uc_switch();
    }
#endif /*FHSS_CHANNEL_DEBUG_CBS*/
}

static int fhss_ws_tx_handle_callback(const fhss_api_t *api, bool is_broadcast_addr, uint8_t *destination_address, int frame_type, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length, uint32_t tx_time)
{
    tr_warn("TX handle callback");
    (void) frame_type;
    (void) frame_length;
    (void) phy_header_length;
    (void) phy_tail_length;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return -1;
    }
    // LFN parent: a frame for the LFN broadcast slot goes on that slot's
    // channel, whatever our own schedule (released for the slot by
    // fhss_ws_lfn_tx_check_callback())
    if (frame_type == FHSS_LFN_BC_FRAME) {
        struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
        if (!lfn->bc_enabled || !fhss_ws_lfn_bc_sched(fhss_structure, tx_time)) {
            return -1;
        }
        uint16_t slot = fhss_lfn_sched_slot_at(&lfn->bc, tx_time - lfn->parent_cfg.tx_offset_us + lfn->bc.interval_us / 2, NULL);
        int32_t channel = fhss_ws_lfn_bc_channel(fhss_structure, slot, lfn->bc_cfg.bsi, fhss_structure->ws->fhss_configuration.ws_bc_channel_function,
                                                 fhss_structure->ws->fhss_configuration.broadcast_fixed_channel);
        if (channel < 0) {
            return -1;
        }
        tr_info("LFN broadcast TX channel: %"PRIi32" slot %u", channel, slot);
        fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, channel);
        return 0;
    }
    // LFN parent: a frame to an LFN goes on the LFN's channel for the slot of
    // its TX time, whatever our own TX/RX slots and broadcast dwell (the
    // frame was released for that slot, fhss_ws_lfn_tx_check_callback()).
    if (!is_broadcast_addr && fhss_structure->ws->lfn.parent_enabled && (fhss_structure->fhss_state == FHSS_SYNCHRONIZED)) {
        fhss_ws_neighbor_timing_info_t *lfn_info = fhss_ws_lfn_neighbor(fhss_structure, destination_address);
        if (lfn_info) {
            fhss_lfn_sched_t sched;
            if (fhss_ws_lfn_neighbor_sched(fhss_structure, lfn_info, &sched)) {
                // The slot whose start is nearest to tx_time - tx_offset:
                // the MAC's first CCA may come a little before the TX time
                // (multi CSMA-CA), the TX itself a little after it
                uint16_t lfn_slot = fhss_lfn_sched_slot_at(&sched, tx_time - fhss_structure->ws->lfn.parent_cfg.tx_offset_us + sched.interval_us / 2, NULL);
                int32_t lfn_channel = fhss_ws_lfn_neighbor_channel(fhss_structure, lfn_info, lfn_slot, destination_address);
                if (lfn_channel < 0) {
                    return -1;
                }
                tr_info("LFN TX channel: %"PRIi32" slot %u %s", lfn_channel, lfn_slot, trace_array(destination_address, 8));
                fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, lfn_channel);
                return 0;
            }
        }
    }
    // LFN: no broadcast schedule of its own; anything else goes as usual
    if (is_broadcast_addr && fhss_structure->ws->lfn.listen_running) {
        return 0;
    }
    // LFN: a unicast (to its parent) must not meet the parent's broadcast
    // dwell; back to the queue until the dwell ends
    if (!is_broadcast_addr && fhss_structure->ws->lfn.listen_running &&
            fhss_ws_lfn_uplink_blocked(fhss_structure, tx_time, frame_length, phy_header_length, phy_tail_length)) {
        return -3;
    }
    // Allow broadcast destination on broadcast channel
    if (is_broadcast_addr && (fhss_structure->ws->is_on_bc_channel == true)) {
        return 0;
    }
    // Do not allow broadcast destination on unicast channel
    if (is_broadcast_addr && (fhss_structure->ws->is_on_bc_channel == false)) {
        return -3;
    }
    // Do not allow unicast destination on broadcast channel unless it is specifically enabled
    if (!is_broadcast_addr && (fhss_structure->ws->is_on_bc_channel == true)) {
        if (fhss_allow_unicast_on_broadcast_channel(fhss_structure)) {
            return 0;
        }
        return -3;
    }
    // Check TX/RX slot
    if (!fhss_allow_transmitting_on_rx_slot(fhss_structure) && !fhss_ws_check_tx_allowed(fhss_structure)) {
        return -1;
    }
    if (fhss_structure->fhss_state == FHSS_SYNCHRONIZED) {
        fhss_ws_neighbor_timing_info_t *neighbor_timing_info = fhss_structure->ws->get_neighbor_info(api, destination_address);
        if (!neighbor_timing_info || neighbor_timing_info->uc_timing_info.unicast_number_of_channels == 0) {
            fhss_stats_update(fhss_structure, STATS_FHSS_UNKNOWN_NEIGHBOR, 1);
            return -2;
        }

        uint16_t destination_slot = fhss_ws_calculate_destination_slot(neighbor_timing_info, tx_time);
        int32_t tx_channel = neighbor_timing_info->uc_timing_info.fixed_channel;
        if (neighbor_timing_info->uc_timing_info.unicast_channel_function == WS_TR51CF) {
            tx_channel = tr51_get_uc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table, destination_slot, destination_address, neighbor_timing_info->uc_timing_info.unicast_number_of_channels, NULL);
            tx_channel = fhss_channel_index_from_mask(neighbor_timing_info->uc_channel_list.channel_mask, tx_channel, fhss_structure->number_of_channels);
        } else if (neighbor_timing_info->uc_timing_info.unicast_channel_function == WS_DH1CF) {
            tx_channel = dh1cf_get_uc_channel_index(destination_slot, destination_address, neighbor_timing_info->uc_channel_list.channel_count);
            tx_channel = fhss_channel_index_from_mask(neighbor_timing_info->uc_channel_list.channel_mask, tx_channel, fhss_structure->number_of_channels);
        } else if (neighbor_timing_info->uc_timing_info.unicast_channel_function == WS_VENDOR_DEF_CF) {
            if (fhss_structure->ws->fhss_configuration.vendor_defined_cf) {
                tx_channel = fhss_structure->ws->fhss_configuration.vendor_defined_cf(fhss_structure->fhss_api, fhss_structure->ws->bc_slot, destination_address, fhss_structure->ws->fhss_configuration.bsi, neighbor_timing_info->uc_timing_info.unicast_number_of_channels);
            } else {
                tr_err("FHSS: vendor defined configuration failed");
                return -1;
            }
        }
//#ifdef FHSS_CHANNEL_DEBUG
#if 1
        tr_warn("TX channel: %u %u %s", tx_channel, destination_slot + 1, trace_array(destination_address, 8));
#endif /*FHSS_CHANNEL_DEBUG*/
        fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, tx_channel);
    }
    return 0;
}

static bool fhss_check_bad_channel(fhss_structure_t *fhss_structure, uint8_t handle)
{
    // When operating on fixed channel, we must ignore the bad channel check.
    if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_FIXED_CHANNEL) {
        return true;
    }
    fhss_failed_tx_t *failed_tx = fhss_failed_handle_find(fhss_structure, handle);
    if (!failed_tx) {
        return true;
    }
    if (failed_tx->bad_channel == fhss_structure->rx_channel) {
        return false;
    }
    return true;
}

static bool fhss_ws_check_tx_allowed(fhss_structure_t *fhss_structure)
{
    // Ignore TX/RX slots
    if (fhss_structure->own_hop == 0xff) {
        //tr_debug("FHSS: TX allowed, own hop is 0xff");
        return true;
    }
    // Currently on broadcast channel
    if (fhss_structure->ws->is_on_bc_channel == true) {
        return true;
    }
    uint32_t number_of_tx_slots = fhss_get_number_of_tx_slots(fhss_structure);
    // Allow transmission when broadcast interval is very short comparing to MAX slot length
    if (!number_of_tx_slots) {
        return true;
    }

    uint32_t remaining_time_ms = get_remaining_slots_us(fhss_structure, fhss_broadcast_handler, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval)) / 1000;
    uint32_t tx_slot_begin_ms = (fhss_structure->ws->fhss_configuration.fhss_broadcast_interval - fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) - (fhss_structure->ws->txrx_slot_length_ms * (fhss_structure->own_hop & 1));
    /* Return false when our first TX slot has not yet started
     *         |------remaining_time_ms---|
     *         |
     *         v
     * | BC | RX | TX | RX | TX | RX | TX | BC |
     */
    if (tx_slot_begin_ms < remaining_time_ms) {
        tr_debug("FHSS: TX slot has not yet started");
        return false;
    }
    tx_slot_begin_ms = tx_slot_begin_ms - (((tx_slot_begin_ms - remaining_time_ms) / (2 * fhss_structure->ws->txrx_slot_length_ms)) * (2 * fhss_structure->ws->txrx_slot_length_ms));
    uint32_t rx_slot_begin_ms = tx_slot_begin_ms - fhss_structure->ws->txrx_slot_length_ms;
    /* Check if we are currently on TX slot.
     *              |        |        |
     *              v        v        v
     * | BC | RX | TX | RX | TX | RX | TX | BC |
     */
    if ((remaining_time_ms <= tx_slot_begin_ms) && (remaining_time_ms > rx_slot_begin_ms)) {
        //tr_debug("FHSS: Currently on TX slot");
        return true;
    }

    tr_debug("FHSS: Currently on RX slot");
    return false;
}

static bool fhss_ws_check_tx_time(fhss_structure_t *fhss_structure, uint16_t tx_length, uint8_t phy_header_length, uint8_t phy_tail_length)
{
    /*
     * Check if there is enough time for transmitting before the next multicast slot.
     */
#ifdef FHSS_WS_PROTECT_MC_SLOTS
    if (!fhss_structure->ws->fhss_configuration.fhss_broadcast_interval || !fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) {
        return true;
    }
    uint32_t tx_time = fhss_get_tx_time(fhss_structure, tx_length, phy_header_length, phy_tail_length);
    uint32_t time_to_bc_channel_us = get_remaining_slots_us(fhss_structure, fhss_broadcast_handler, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval));
    if (tx_time > time_to_bc_channel_us) {
        return false;
    }
    return true;
#else
    (void) fhss_structure;
    (void) tx_length;
    (void) phy_header_length;
    (void) phy_tail_length;
    return true;
#endif
}

static bool fhss_allow_transmitting_on_rx_slot(fhss_structure_t *fhss_structure)
{
    if (fhss_structure->ws->tx_level >= WS_TX_AND_RX_SLOT) {
        return true;
    }
    // This is allowed only for devices in expedited forwarding mode
    if (fhss_structure->ws->expedited_forwarding_enabled_us && (fhss_structure->ws->ef_tx_level >= WS_TX_AND_RX_SLOT)) {
        return true;
    }
    return false;
}

static bool fhss_allow_unicast_on_broadcast_channel(fhss_structure_t *fhss_structure)
{
    if (fhss_structure->ws->tx_level >= WS_TX_ALWAYS) {
        return true;
    }
    // This is allowed only for devices in expedited forwarding mode
    if (fhss_structure->ws->expedited_forwarding_enabled_us && (fhss_structure->ws->ef_tx_level >= WS_TX_ALWAYS)) {
        return true;
    }
    return false;
}

static bool fhss_ws_check_tx_conditions_callback(const fhss_api_t *api, bool is_broadcast_addr, uint8_t handle, int frame_type, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length)
{
    (void) frame_type;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return true;
    }
    // LFN: no broadcast schedule to wait for
    if (is_broadcast_addr && fhss_structure->ws->lfn.listen_running) {
        return true;
    }
    // LFN: a unicast waits for the end of the parent's broadcast dwell
    if (!is_broadcast_addr && fhss_structure->ws->lfn.listen_running &&
            fhss_ws_lfn_uplink_blocked(fhss_structure, fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api),
                                       frame_length, phy_header_length, phy_tail_length)) {
        return false;
    }
    // Do not allow broadcast destination on unicast channel
    if (is_broadcast_addr && (fhss_structure->ws->is_on_bc_channel == false)) {
        return false;
    }
    // Do not allow unicast destination on broadcast channel unless it is specifically enabled
    if (!is_broadcast_addr && (fhss_structure->ws->is_on_bc_channel == true) && !fhss_allow_unicast_on_broadcast_channel(fhss_structure)) {
        return false;
    }
    // This condition will check that message is not sent on bad channel
    if (fhss_check_bad_channel(fhss_structure, handle) == false) {
        return false;
    }
    // Check that there is enough unicast TX time before next broadcast channel. We try to avoid delaying the change to broadcast channel because of ongoing transmission.
    if (!is_broadcast_addr && !fhss_ws_check_tx_time(fhss_structure, frame_length, phy_header_length, phy_tail_length)) {
        return false;
    }
    // Check TX/RX slot for unicast frames
    if (!is_broadcast_addr && !fhss_allow_transmitting_on_rx_slot(fhss_structure) && !fhss_ws_check_tx_allowed(fhss_structure)) {
        return false;
    }
    return true;
}

static uint8_t fhss_ws_ie_header_discover(uint8_t *header_ptr, uint16_t length, struct ws_ie_t *header_ie, uint8_t sub_id)
{
    struct ws_ie_t ie_element;
    uint8_t *sub_id_ptr;
    uint16_t ie_dummy;
    while (length > 2) {
        ie_dummy = common_read_16_bit_inverse(header_ptr);
        ie_element.length = (ie_dummy & IE_HEADER_LENGTH_MASK);
        ie_element.id = ((ie_dummy & IE_HEADER_ID_MASK) >> 7);
        ie_element.content_ptr = header_ptr + 2;
        sub_id_ptr = ie_element.content_ptr;
        if (ie_element.length && (header_ie->id == ie_element.id) && (*sub_id_ptr == sub_id)) {
            sub_id_ptr++;
            ie_element.length--;
            header_ie->content_ptr = sub_id_ptr;
            header_ie->length = ie_element.length;
            return ie_element.length;
        }
        length -= ie_element.length + 2;
        header_ptr += ie_element.length + 2;
    }
    return 0;
}

static int16_t fhss_ws_write_synch_info_callback(const fhss_api_t *api, uint8_t *ptr, uint8_t length, int frame_type, uint32_t tx_time)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure || !ptr || (frame_type != FHSS_DATA_FRAME)) {
        return -1;
    }
    platform_enter_critical();
    struct ws_ie_t header_ie;
    header_ie.id = WH_IE_ID;
    if (fhss_ws_ie_header_discover(ptr, length, &header_ie, WH_SUB_ID_UTT)) {
        uint32_t ufsi = fhss_ws_calculate_ufsi(fhss_structure, tx_time);
        common_write_24_bit_inverse(ufsi, header_ie.content_ptr + 1);
    }
    if (fhss_ws_ie_header_discover(ptr, length, &header_ie, WH_SUB_ID_BT)) {
        uint32_t broadcast_interval_offset = fhss_ws_calculate_broadcast_interval_offset(fhss_structure, tx_time);
        common_write_16_bit_inverse(fhss_structure->ws->bc_slot, header_ie.content_ptr);
        common_write_24_bit_inverse(broadcast_interval_offset, header_ie.content_ptr + 2);
    }
    // LFN: own slot number and the offset into it (ms, rounded) at TX time
    // (LUTT-IE: frame type, slot number, interval offset), only while the
    // listen schedule runs; otherwise the LLC's zeros stay.
    if (fhss_structure->ws->lfn.listen_running && fhss_ws_ie_header_discover(ptr, length, &header_ie, WH_SUB_ID_LUTT) >= 6) {
        uint32_t offset_us;
        uint16_t slot = fhss_lfn_sched_slot_at(&fhss_structure->ws->lfn.own, tx_time, &offset_us);
        uint32_t offset_ms = (offset_us + 500) / 1000;
        if (offset_ms >= fhss_structure->ws->lfn.listen_cfg.listen_interval_ms) {
            offset_ms = fhss_structure->ws->lfn.listen_cfg.listen_interval_ms - 1;
        }
        common_write_16_bit_inverse(slot, header_ie.content_ptr + 1);
        common_write_24_bit_inverse(offset_ms, header_ie.content_ptr + 3);
    }
    if (fhss_structure->ws->lfn.bc_enabled) {
        fhss_ws_lfn_write_lbt_lto(fhss_structure, ptr, length, tx_time);
    }
    platform_exit_critical();
    //TODO return destination channel here
    return fhss_structure->rx_channel;
}

static void fhss_ws_data_tx_done_callback(const fhss_api_t *api, bool waiting_ack, bool tx_completed, uint8_t handle)
{
    tr_warn("fhss_ws_data_tx_done_callback: waiting_ack=%d, tx_completed=%d, handle=%d", waiting_ack, tx_completed, handle);
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return;
    }
    if (fhss_structure->ws->lfn.listen_running && !waiting_ack && !tx_completed) {
        // LFN: a failed frame may wait in the MAC queue for its restart; with
        // no periodic poll, poll until the queue is empty (fhss_event_timer_cb)
        eventOS_callback_timer_start(fhss_structure->fhss_event_timer, FHSS_LFN_QUEUE_POLL_US / 50);
    }
    if (fhss_structure->fhss_state == FHSS_SYNCHRONIZED) {
        if (waiting_ack == false) {
            if (fhss_structure->ws->is_on_bc_channel == false) {
                fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, fhss_structure->rx_channel);
            } else {
                fhss_structure->callbacks.change_channel(fhss_structure->fhss_api, fhss_structure->ws->bc_channel);
            }
        }
    }
    // Buffer was successfully transmitted. Remove stored failure handle if exists.
    if (tx_completed == true) {
        fhss_failed_tx_t *fhss_failed_tx = fhss_failed_handle_find(fhss_structure, handle);
        if (fhss_failed_tx) {
            fhss_failed_handle_remove(fhss_structure, handle);
        }
    }
}

static bool fhss_ws_data_tx_fail_callback(const fhss_api_t *api, uint8_t handle, int frame_type, uint8_t channel)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return false;
    }
    // Only use channel retries when device is synchronized
    if (fhss_structure->fhss_state == FHSS_UNSYNCHRONIZED) {
        return false;
    }
    // Use channel retries only for data frames
    if (FHSS_DATA_FRAME != frame_type) {
        return false;
    }
    // Channel retries are disabled
    if (!fhss_structure->ws->fhss_configuration.config_parameters.number_of_channel_retries) {
        return false;
    }
    fhss_failed_tx_t *fhss_failed_tx = fhss_failed_handle_find(fhss_structure, handle);
    if (fhss_failed_tx) {
        fhss_failed_tx->retries_done++;
        if (fhss_failed_tx->retries_done >= fhss_structure->ws->fhss_configuration.config_parameters.number_of_channel_retries) {
            // No more retries. Return false to stop retransmitting.
            fhss_failed_handle_remove(fhss_structure, handle);
            return false;
        }
        fhss_failed_tx->bad_channel = channel;
    } else {
        // Create new failure handle and return true to retransmit
        fhss_failed_handle_add(fhss_structure, handle, fhss_structure->rx_channel);
    }
    fhss_stats_update(fhss_structure, STATS_FHSS_CHANNEL_RETRY, 1);
    return true;
}

static void fhss_ws_receive_frame_callback(const fhss_api_t *api, uint16_t pan_id, uint8_t *source_address, uint32_t timestamp, uint8_t *synch_info, int frame_type)
{
    (void) api;
    (void) pan_id;
    (void) source_address;
    (void) timestamp;
    (void) synch_info;
    (void) frame_type;
}

static bool fhss_ws_is_broadcast_channel_callback(const fhss_api_t *api)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return true;
    }
    return fhss_structure->ws->is_on_bc_channel;
}

static bool fhss_ws_use_broadcast_queue_cb(const fhss_api_t *api, bool is_broadcast_addr, int frame_type)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return false;
    }
    // LFN broadcast frames wait in the unicast queue for their slot
    if (frame_type == FHSS_LFN_BC_FRAME) {
        return false;
    }
    // Broadcast packets are stored in broadcast queue
    return is_broadcast_addr;
}

static uint32_t fhss_ws_get_retry_period_callback(const fhss_api_t *api, uint8_t *destination_address, uint16_t phy_mtu)
{
    (void) destination_address;
    (void) phy_mtu;
    uint32_t return_value = 0;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure) {
        return return_value;
    }
    // We don't know the broadcast schedule, use randomised large backoff with MAC retries
    if (fhss_structure->ws->broadcast_timer_running == false) {
        return (uint32_t) rand_get_random_in_range(20000, 45000);
    }
    // We don't know the TX/RX slots, use randomised large backoff with MAC retries
    if (fhss_structure->own_hop == 0xff) {
        return (uint32_t) rand_get_random_in_range(20000, 45000);
    }
    if (fhss_structure->ws->is_on_bc_channel == true) {
        return return_value;
    }
    // No need to wait until TX slot when sending on RX slot is allowed
    if (fhss_allow_transmitting_on_rx_slot(fhss_structure) == true) {
        return return_value;
    }

    uint32_t txrx_slot_length_us = MS_TO_US(fhss_structure->ws->txrx_slot_length_ms);
    uint32_t unicast_start_us = fhss_structure->ws->unicast_start_time_us;
    uint32_t cur_time_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    uint32_t tx_trig_offset_us = (txrx_slot_length_us / 2) * calc_own_tx_trig_slot(fhss_structure->own_hop);

    uint32_t next_tx_trig_slot_start_us = unicast_start_us + (txrx_slot_length_us * (fhss_structure->own_hop & 1)) + tx_trig_offset_us;
    uint32_t next_tx_trig_slot_end_us = next_tx_trig_slot_start_us + (txrx_slot_length_us / 2);
    while ((next_tx_trig_slot_start_us < cur_time_us) || ((next_tx_trig_slot_start_us - cur_time_us) > (uint32_t) MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval))) {
        if (cur_time_us < next_tx_trig_slot_end_us) {
            return 0;
        }
        next_tx_trig_slot_start_us += (txrx_slot_length_us * 2);
        next_tx_trig_slot_end_us = next_tx_trig_slot_start_us + (txrx_slot_length_us / 2);
    }
    return_value = next_tx_trig_slot_start_us - cur_time_us;
    uint32_t time_to_bc_channel = get_remaining_slots_us(fhss_structure, fhss_broadcast_handler, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_broadcast_interval));
    if (time_to_bc_channel <= return_value) {
        return_value += MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval);
    }

    return return_value;
}

static void fhss_unicast_handler(const fhss_api_t *fhss_api, uint16_t delay)
{
    (void) delay;
    uint32_t timeout = 0;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || fhss_structure->ws->lfn.listen_running) {
        return;
    }
    platform_enter_critical();
    int32_t delay_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) - fhss_structure->ws->next_uc_timeout;
    if (!fhss_structure->ws->uc_slot && !fhss_structure->ws->next_uc_timeout) {
        delay_us = 0;
    }

    // If callback was delayed longer than dwell time, UC slot must be updated
    if ((fhss_structure->ws->fhss_configuration.ws_uc_channel_function != WS_FIXED_CHANNEL)) {
        fhss_structure->ws->uc_slot += US_TO_MS(delay_us) / fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval;
        if (fhss_structure->ws->fhss_configuration.ws_uc_channel_function == WS_TR51CF) {
            fhss_structure->ws->uc_slot %= fhss_structure->number_of_uc_channels;
        }
    }
    delay_us %= MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval);


    timeout = fhss_ws_get_sf_timeout_callback(fhss_structure);
    if (!timeout) {
        fhss_stop_timer(fhss_structure, fhss_unicast_handler);
        fhss_structure->ws->unicast_timer_running = false;
        platform_exit_critical();
        return;
    }
    fhss_ws_start_timer(fhss_structure, timeout - (delay_us * fhss_structure->platform_functions.fhss_resolution_divider), fhss_unicast_handler);
    fhss_structure->ws->unicast_timer_running = true;
    fhss_ws_update_uc_channel_callback(fhss_structure);
    // Unless we have broadcast schedule, we have to poll unicast queue when changing channel. This is randomized by the unicast schedule.
    if (!fhss_structure->ws->fhss_configuration.fhss_broadcast_interval || !fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval) {
        if (fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false)) {
            fhss_structure->callbacks.tx_poll(fhss_structure->fhss_api);
        }
    }
    platform_exit_critical();
}

int fhss_ws_set_callbacks(fhss_structure_t *fhss_structure)
{
    // Set external API
    fhss_structure->fhss_api->is_broadcast_channel = &fhss_ws_is_broadcast_channel_callback;
    fhss_structure->fhss_api->use_broadcast_queue = &fhss_ws_use_broadcast_queue_cb;
    fhss_structure->fhss_api->tx_handle = &fhss_ws_tx_handle_callback;
    fhss_structure->fhss_api->check_tx_conditions = &fhss_ws_check_tx_conditions_callback;
    fhss_structure->fhss_api->receive_frame = &fhss_ws_receive_frame_callback;
    fhss_structure->fhss_api->data_tx_done = &fhss_ws_data_tx_done_callback;
    fhss_structure->fhss_api->data_tx_fail = &fhss_ws_data_tx_fail_callback;
    fhss_structure->fhss_api->synch_state_set = &fhss_ws_synch_state_set_callback;
    fhss_structure->fhss_api->read_timestamp = NULL;
    fhss_structure->fhss_api->get_retry_period = &fhss_ws_get_retry_period_callback;
    fhss_structure->fhss_api->write_synch_info = &fhss_ws_write_synch_info_callback;
    fhss_structure->fhss_api->init_callbacks = &fhss_init_callbacks_cb;
    fhss_structure->fhss_api->lfn_tx_check = &fhss_ws_lfn_tx_check_callback;
    return 0;
}

int fhss_ws_set_parent(fhss_structure_t *fhss_structure, const uint8_t eui64[8], const broadcast_timing_info_t *bc_timing_info, const bool force_synch)
{
    (void) eui64;
    if (!fhss_structure->ws) {
        return -1;
    }
    // LFN: it does not follow an FFN broadcast schedule; it keeps the
    // parent's, so that its uplinks avoid the parent's broadcast dwell
    if (fhss_structure->ws->lfn.listen_enabled) {
        struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
        if (!bc_timing_info->broadcast_interval || !bc_timing_info->broadcast_dwell_interval ||
                bc_timing_info->broadcast_dwell_interval >= bc_timing_info->broadcast_interval) {
            lfn->parent_bc_valid = false;
            return 0;
        }
        lfn->parent_bc_interval_us = MS_TO_US(bc_timing_info->broadcast_interval);
        lfn->parent_bc_dwell_us = MS_TO_US(bc_timing_info->broadcast_dwell_interval);
        lfn->parent_bc_start_us = bc_timing_info->bt_rx_timestamp - MS_TO_US(bc_timing_info->broadcast_interval_offset);
        lfn->parent_bc_valid = true;
        return 0;
    }
    if (!bc_timing_info->broadcast_interval || !bc_timing_info->broadcast_dwell_interval) {
        return -1;
    }
    if (((uint32_t)S_TO_US(fhss_structure->ws->min_synch_interval) > (fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) - fhss_structure->ws->synchronization_time)) && !force_synch) {
        return 0;
    }
    // The synchronisation trace is done after the critical section, from
    // values captured inside it (see fhss_broadcast_handler()).
    bool trace_synch = false;
    int32_t trace_drift_ms = 0;
    platform_enter_critical();

    uint16_t own_bc_slot = fhss_structure->ws->bc_slot;
    uint32_t prev_synchronization_time = fhss_structure->ws->synchronization_time;
    fhss_structure->ws->synchronization_time = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    uint32_t time_since_last_synch_us = fhss_structure->ws->synchronization_time - prev_synchronization_time;
    uint32_t own_bc_interval_offset = fhss_ws_calculate_broadcast_interval_offset(fhss_structure, fhss_structure->ws->synchronization_time);
    fhss_stop_timer(fhss_structure, fhss_broadcast_handler);
    uint32_t time_from_reception_ms = US_TO_MS(fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) - bc_timing_info->bt_rx_timestamp);
    uint32_t true_bc_interval_offset = (bc_timing_info->broadcast_interval_offset + time_from_reception_ms) % bc_timing_info->broadcast_interval;
    if (true_bc_interval_offset >= bc_timing_info->broadcast_dwell_interval) {
        fhss_structure->ws->is_on_bc_channel = false;
    }
    uint32_t timeout = MS_TO_US(bc_timing_info->broadcast_interval - true_bc_interval_offset);

    if (fhss_structure->ws->is_on_bc_channel) {
        timeout -= MS_TO_US(bc_timing_info->broadcast_interval - bc_timing_info->broadcast_dwell_interval);
    }
    fhss_ws_start_timer(fhss_structure, timeout, fhss_broadcast_handler);
    fhss_structure->ws->broadcast_timer_running = true;
    uint16_t slots_since_reception = (bc_timing_info->broadcast_interval_offset + time_from_reception_ms) / bc_timing_info->broadcast_interval;
    // TODO: Calculate drift error
    fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval = bc_timing_info->broadcast_dwell_interval;
    fhss_structure->ws->fhss_configuration.fhss_broadcast_interval = bc_timing_info->broadcast_interval;
    fhss_set_txrx_slot_length(fhss_structure);
    if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_FIXED_CHANNEL) {
        fhss_structure->ws->bc_slot = 0;
    } else {
        fhss_structure->ws->bc_slot = bc_timing_info->broadcast_slot + slots_since_reception;
    }
    if (fhss_structure->ws->fhss_configuration.ws_bc_channel_function == WS_TR51CF) {
        fhss_structure->ws->bc_slot %= fhss_structure->number_of_channels;
    }
    //TODO: support multiple parents
    if (prev_synchronization_time && fhss_structure->ws->fhss_configuration.ws_bc_channel_function != WS_FIXED_CHANNEL) {
        //TODO: Compensation for fixed channel configuration
        if (SYNCH_COMPENSATION_MIN_INTERVAL <= US_TO_S(time_since_last_synch_us)) {
            // Update clock drift
            int32_t drift_per_ms_tmp = divide_integer((int32_t)MS_TO_NS((true_bc_interval_offset - own_bc_interval_offset) + ((int32_t)(fhss_structure->ws->bc_slot - own_bc_slot) * bc_timing_info->broadcast_interval)), US_TO_MS(time_since_last_synch_us));
            if (drift_per_ms_tmp > MAX_DRIFT_COMPENSATION_STEP) {
                drift_per_ms_tmp = MAX_DRIFT_COMPENSATION_STEP;
            } else if (drift_per_ms_tmp < -MAX_DRIFT_COMPENSATION_STEP) {
                drift_per_ms_tmp = -MAX_DRIFT_COMPENSATION_STEP;
            }
            // Timer drift is unpredictable with linux platform. Do not set drift compensation.
            // The simulation port's clocks are exact, with the drift the
            // conductor gives each node (EM3 step 5): compensate there.
#if !defined(__linux__) || defined(NS_PORT_SIM)
            fhss_structure->ws->drift_per_millisecond_ns += drift_per_ms_tmp;
#endif
            fhss_stats_update(fhss_structure, STATS_FHSS_DRIFT_COMP, NS_TO_US((int64_t)(fhss_structure->ws->drift_per_millisecond_ns * bc_timing_info->broadcast_dwell_interval)));
        }
        trace_synch = true;
        trace_drift_ms = true_bc_interval_offset - own_bc_interval_offset + ((int32_t)(fhss_structure->ws->bc_slot - own_bc_slot) * bc_timing_info->broadcast_interval);
    }
    int32_t trace_compensation = fhss_structure->ws->drift_per_millisecond_ns;
    platform_exit_critical();
    if (trace_synch) {
        tr_debug("synch to parent: %s, drift: %"PRIi32"ms in %"PRIu64" seconds, compensation: %"PRIi32"ns per ms", trace_array(eui64, 8), trace_drift_ms, US_TO_S(time_since_last_synch_us), trace_compensation);
    }
    fhss_stats_update(fhss_structure, STATS_FHSS_SYNCH_INTERVAL, US_TO_S(time_since_last_synch_us));
    return 0;
}

int fhss_ws_remove_parent(fhss_structure_t *fhss_structure, const uint8_t eui64[8])
{
    (void) eui64;
    if (!fhss_structure->ws) {
        return -1;
    }
    return 0;
}

int fhss_ws_configuration_set(fhss_structure_t *fhss_structure, const fhss_ws_configuration_t *fhss_configuration)
{
    int channel_count_bc = channel_list_count_channels(fhss_configuration->channel_mask);
    int channel_count_uc = channel_list_count_channels(fhss_configuration->unicast_channel_mask);
    if (channel_count_bc <= 0 || fhss_configuration->channel_mask_size == 0) {
        return -1;
    }

    if (fhss_structure->number_of_bc_channels < channel_count_bc ||
            (channel_count_uc && fhss_structure->number_of_uc_channels < channel_count_uc)) {
        // Channel amount changed to largeneed to reallocate channel table
        free(fhss_structure->ws->tr51_channel_table);
        fhss_structure->ws->tr51_channel_table = NULL;
        free(fhss_structure->ws->tr51_output_table);
        fhss_structure->ws->tr51_output_table = NULL;

        if (fhss_ws_manage_channel_table_allocation(fhss_structure, channel_count_uc > channel_count_bc ? channel_count_uc : channel_count_bc)) {
            return -1;
        }
    }
    platform_enter_critical();
    if (fhss_configuration->ws_uc_channel_function == WS_FIXED_CHANNEL || fhss_configuration->fhss_uc_dwell_interval == 0) {
        fhss_stop_timer(fhss_structure, fhss_unicast_handler);
        fhss_structure->ws->unicast_timer_running = false;
    }
    if (fhss_configuration->ws_bc_channel_function == WS_FIXED_CHANNEL || (fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval == 0 || fhss_structure->ws->fhss_configuration.fhss_broadcast_interval == 0)) {
        fhss_structure->ws->synchronization_time = 0;
    }

    if (!fhss_structure->ws->lfn.listen_running && (fhss_structure->ws->unicast_timer_running == false) && (fhss_configuration->ws_uc_channel_function != WS_FIXED_CHANNEL) && fhss_configuration->fhss_uc_dwell_interval) {
        fhss_ws_start_timer(fhss_structure, MS_TO_US(fhss_configuration->fhss_uc_dwell_interval), fhss_unicast_handler);
        fhss_structure->ws->unicast_timer_running = true;
    }
    fhss_structure->ws->fhss_configuration = *fhss_configuration;
    if (channel_count_uc == 0) {
        //If Unicast channel is empty use Domain mask
        for (uint8_t i = 0; i < 8; i++) {
            fhss_structure->ws->fhss_configuration.unicast_channel_mask[i] = fhss_configuration->channel_mask[i];
        }
        channel_count_uc = channel_count_bc;
    }

    // No one would poll TX queue if schedule timers were not started. Start poll timer with default interval.
    if (!fhss_structure->ws->lfn.listen_running && (fhss_structure->ws->unicast_timer_running == false) && (fhss_structure->ws->broadcast_timer_running == false)) {
        eventOS_callback_timer_start(fhss_structure->fhss_event_timer, DEFAULT_POLL_PERIOD);
    }

    fhss_structure->number_of_channels = fhss_configuration->channel_mask_size;
    fhss_structure->number_of_bc_channels = channel_count_bc;
    fhss_structure->number_of_uc_channels = channel_count_uc;
    if (fhss_configuration->ws_uc_channel_function == WS_FIXED_CHANNEL) {
        fhss_structure->rx_channel = fhss_configuration->unicast_fixed_channel;
    }
    platform_exit_critical();
    tr_info("fhss Configuration set, UC channel: %d, BC channel: %d, UC CF: %d, BC CF: %d, channels: BC %d UC %d, uc dwell: %d, bc dwell: %d, bc interval: %"PRIu32", bsi:%d, ch retries: %u",
            fhss_structure->ws->fhss_configuration.unicast_fixed_channel,
            fhss_structure->ws->fhss_configuration.broadcast_fixed_channel,
            fhss_structure->ws->fhss_configuration.ws_uc_channel_function,
            fhss_structure->ws->fhss_configuration.ws_bc_channel_function,
            fhss_structure->number_of_bc_channels,
            fhss_structure->number_of_uc_channels,
            fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval,
            fhss_structure->ws->fhss_configuration.fhss_bc_dwell_interval,
            fhss_structure->ws->fhss_configuration.fhss_broadcast_interval,
            fhss_structure->ws->fhss_configuration.bsi,
            fhss_structure->ws->fhss_configuration.config_parameters.number_of_channel_retries);

    return 0;
}

int fhss_ws_set_hop_count(fhss_structure_t *fhss_structure, const uint8_t hop_count)
{
    fhss_structure->own_hop = hop_count;
    fhss_stats_update(fhss_structure, STATS_FHSS_HOP_COUNT, fhss_structure->own_hop);
    return 0;
}

int fhss_ws_set_tx_allowance_level(fhss_structure_t *fhss_structure, const fhss_ws_tx_allow_level global_level, const fhss_ws_tx_allow_level ef_level)
{
    fhss_structure->ws->tx_level = global_level;
    fhss_structure->ws->ef_tx_level = ef_level;
    return 0;
}

bool fhss_synch_global_state_get() {
    return fhss_synch_global_state;
}

/* ------------------------------------------------------------------------
 * Wi-SUN FAN 1.1 LFN unicast mechanics (GS8 stage 2, docs/LFN_MAC.md).
 *
 * LFN role: the node's own unicast listen schedule. The FFN unicast hopping
 * and broadcast schedule stop; one FHSS timer opens a receive window around
 * the start of each slot, on the channel of the node's unicast channel
 * function for that slot number, and closes it (the MAC turns the radio on
 * and off: callbacks.rx_window).
 *
 * Parent role: frames to a neighbour with an LFN unicast schedule are held
 * in the MAC queue and released for the LFN's next slot (lfn_tx_check); the
 * same timer polls the queue just before.
 * ------------------------------------------------------------------------ */

// Channel of a unicast channel function at a slot, for a node's address,
// with the same rules as the FFN schedule (fhss_ws_update_uc_channel_callback
// for the own schedule, fhss_ws_tx_handle_callback for a neighbour's).
static int32_t fhss_ws_lfn_own_channel(fhss_structure_t *fhss_structure, uint16_t slot)
{
    uint8_t mac_address[8];
    int32_t channel = fhss_structure->ws->fhss_configuration.unicast_fixed_channel;
    const fhss_ws_configuration_t *cfg = &fhss_structure->ws->fhss_configuration;

    fhss_structure->callbacks.read_mac_address(fhss_structure->fhss_api, mac_address);
    if (cfg->ws_uc_channel_function == WS_TR51CF) {
        channel = tr51_get_uc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table,
                                            slot % fhss_structure->number_of_uc_channels, mac_address, fhss_structure->number_of_uc_channels, NULL);
        channel = fhss_channel_index_from_mask(cfg->unicast_channel_mask, channel, fhss_structure->number_of_channels);
    } else if (cfg->ws_uc_channel_function == WS_DH1CF) {
        channel = dh1cf_get_uc_channel_index(slot, mac_address, fhss_structure->number_of_uc_channels);
        channel = fhss_channel_index_from_mask(cfg->unicast_channel_mask, channel, fhss_structure->number_of_channels);
    } else if (cfg->ws_uc_channel_function == WS_VENDOR_DEF_CF && cfg->vendor_defined_cf) {
        channel = cfg->vendor_defined_cf(fhss_structure->fhss_api, slot, mac_address, cfg->bsi, fhss_structure->number_of_uc_channels);
    }
    return channel;
}

static int32_t fhss_ws_lfn_neighbor_channel(fhss_structure_t *fhss_structure, fhss_ws_neighbor_timing_info_t *info, uint16_t slot, uint8_t *eui64)
{
    int32_t channel = info->uc_timing_info.fixed_channel;

    if (info->uc_timing_info.unicast_channel_function == WS_TR51CF) {
        if (!info->uc_timing_info.unicast_number_of_channels) {
            return -1;
        }
        channel = tr51_get_uc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table,
                                            slot % info->uc_timing_info.unicast_number_of_channels, eui64,
                                            info->uc_timing_info.unicast_number_of_channels, NULL);
        channel = fhss_channel_index_from_mask(info->uc_channel_list.channel_mask, channel, fhss_structure->number_of_channels);
    } else if (info->uc_timing_info.unicast_channel_function == WS_DH1CF) {
        if (!info->uc_channel_list.channel_count) {
            return -1;
        }
        channel = dh1cf_get_uc_channel_index(slot, eui64, info->uc_channel_list.channel_count);
        channel = fhss_channel_index_from_mask(info->uc_channel_list.channel_mask, channel, fhss_structure->number_of_channels);
    } else if (info->uc_timing_info.unicast_channel_function == WS_VENDOR_DEF_CF) {
        if (!fhss_structure->ws->fhss_configuration.vendor_defined_cf) {
            return -1;
        }
        channel = fhss_structure->ws->fhss_configuration.vendor_defined_cf(fhss_structure->fhss_api, slot, eui64, fhss_structure->ws->fhss_configuration.bsi,
                                                                             info->uc_timing_info.unicast_number_of_channels);
    }
    return channel;
}

static fhss_ws_neighbor_timing_info_t *fhss_ws_lfn_neighbor(fhss_structure_t *fhss_structure, const uint8_t *eui64)
{
    fhss_ws_neighbor_timing_info_t *info;

    if (!fhss_structure->ws->get_neighbor_info || !eui64) {
        return NULL;
    }
    info = fhss_structure->ws->get_neighbor_info(fhss_structure->fhss_api, (uint8_t *)eui64);
    if (!info || !info->lfn_uc_timing_info.listen_interval_ms) {
        return NULL;
    }
    return info;
}

// The LFN schedule of a neighbour, from its last LUS-IE and LUTT-IE. The
// 32-bit clock limits how old the LUTT may be (2^31 µs): once it is more than
// 2^30 µs old, it is replaced in the neighbour's entry by an equivalent one
// at a recent slot start (offset 0), so a parent that keeps sending to a
// silent LFN keeps its phase.
static bool fhss_ws_lfn_neighbor_sched(fhss_structure_t *fhss_structure, fhss_ws_neighbor_timing_info_t *info, fhss_lfn_sched_t *sched)
{
    lfn_unicast_timing_info_t *lt = &info->lfn_uc_timing_info;
    uint32_t now;

    if (!fhss_lfn_sched_from_lutt(sched, lt->listen_interval_ms, lt->slot_number, lt->interval_offset_ms, lt->lutt_rx_timestamp)) {
        return false;
    }
    now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    if ((uint32_t)(now - lt->lutt_rx_timestamp) > 0x40000000u) {
        fhss_lfn_sched_reanchor(sched, now);
        lt->slot_number = sched->anchor_slot;
        lt->interval_offset_ms = 0;
        lt->lutt_rx_timestamp = sched->anchor_us;
    }
    return true;
}

// Arm the LFN timer in delay_us (at least 1 µs).
static void fhss_ws_lfn_timer_arm(fhss_structure_t *fhss_structure, int32_t delay_us)
{
    fhss_start_timer(fhss_structure, delay_us > 0 ? (uint32_t)delay_us : 1, fhss_ws_lfn_timer_handler);
}

static void fhss_ws_lfn_listen_start(fhss_structure_t *fhss_structure)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    uint32_t now;

    // The FFN schedules stop: no hopping every dwell interval, no broadcast
    // dwell, no periodic TX queue poll
    fhss_stop_timer(fhss_structure, fhss_unicast_handler);
    fhss_stop_timer(fhss_structure, fhss_broadcast_handler);
    eventOS_callback_timer_stop(fhss_structure->fhss_event_timer);
    fhss_structure->ws->unicast_timer_running = false;
    fhss_structure->ws->broadcast_timer_running = false;
    fhss_structure->ws->is_on_bc_channel = false;

    now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    lfn->own.interval_us = fhss_ws_lfn_local_us(lfn, lfn->listen_cfg.listen_interval_ms * 1000);
    lfn->own.anchor_us = now + lfn->listen_cfg.first_slot_delay_us;
    lfn->own.anchor_slot = 0;
    lfn->slot = 0;
    lfn->slot_start = lfn->own.anchor_us;
    lfn->window_open = false;
    lfn->close_retries = 0;
    lfn->listen_running = true;
    fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOWS_START, 0);
    fhss_ws_lfn_timer_arm(fhss_structure, (int32_t)(lfn->slot_start - lfn->listen_cfg.rx_guard_us - now));
    tr_info("LFN listen schedule: interval %"PRIu32" ms, window %"PRIu32" us (guard %"PRIu32" us), slot 0 at %"PRIu32,
            lfn->listen_cfg.listen_interval_ms, lfn->listen_cfg.rx_window_us, lfn->listen_cfg.rx_guard_us, lfn->slot_start);
}

static void fhss_ws_lfn_listen_stop(fhss_structure_t *fhss_structure)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    if (!lfn->listen_running) {
        return;
    }
    fhss_stop_timer(fhss_structure, fhss_ws_lfn_timer_handler);
    fhss_ws_lfn_bc_stop(fhss_structure);
    if (lfn->window_open) {
        fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_CLOSE, 0);
        lfn->window_open = false;
    }
    fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOWS_STOP, 0);
    lfn->listen_running = false;
    lfn->resched = false;
    lfn->parent_bc_valid = false;
    // Back to the FFN unicast hopping. The FFN broadcast schedule resumes at
    // the next ns_fhss_ws_set_parent().
    if (fhss_structure->fhss_state == FHSS_SYNCHRONIZED && fhss_structure->ws->fhss_configuration.ws_uc_channel_function != WS_FIXED_CHANNEL &&
            fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval) {
        fhss_ws_update_uc_channel_callback(fhss_structure);
        fhss_ws_start_timer(fhss_structure, MS_TO_US(fhss_structure->ws->fhss_configuration.fhss_uc_dwell_interval), fhss_unicast_handler);
        fhss_structure->ws->unicast_timer_running = true;
    }
}

// When the window of slot_start opens: rx_guard_us before it, plus the
// widening for drift since the last sync with the parent
static uint32_t fhss_ws_lfn_open_time(struct fhss_ws_lfn *lfn, uint32_t slot_start)
{
    return slot_start - lfn->listen_cfg.rx_guard_us - fhss_ws_lfn_widening(lfn, slot_start);
}

// The next own slot whose window opens after now, from the 'own' schedule
static void fhss_ws_lfn_own_next(fhss_structure_t *fhss_structure, uint32_t now)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    lfn->slot_start = fhss_lfn_sched_next(&lfn->own, now, &lfn->slot);
    while ((int32_t)(fhss_ws_lfn_open_time(lfn, lfn->slot_start) - now) <= 0) {
        lfn->slot++;
        lfn->slot_start += lfn->own.interval_us;
    }
    fhss_ws_lfn_timer_arm(fhss_structure, (int32_t)(fhss_ws_lfn_open_time(lfn, lfn->slot_start) - now));
}

static void fhss_ws_lfn_listen_timer(fhss_structure_t *fhss_structure)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    const fhss_ws_lfn_listen_t *cfg = &lfn->listen_cfg;
    uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    uint32_t widening = fhss_ws_lfn_widening(lfn, lfn->slot_start);
    uint32_t window_end = lfn->slot_start - cfg->rx_guard_us + cfg->rx_window_us + widening;

    if (!lfn->window_open && lfn->bc_window_open) {
        // A broadcast window holds the radio: skip this slot
        lfn->stats.windows_skipped++;
        lfn->own.anchor_us = lfn->slot_start;
        lfn->own.anchor_slot = lfn->slot;
        fhss_ws_lfn_own_next(fhss_structure, now);
        return;
    }
    if (!lfn->window_open) {
        int32_t channel = fhss_ws_lfn_own_channel(fhss_structure, lfn->slot);
        // The own schedule is anchored at the current slot, so the 32-bit
        // clock never limits how long it runs (LUTT-IE at TX time)
        lfn->own.anchor_us = lfn->slot_start;
        lfn->own.anchor_slot = lfn->slot;
        lfn->window_channel = channel;
        // The MAC's TX done returns to rx_channel: the window's channel
        fhss_structure->rx_channel = channel;
        lfn->window_open = true;
        lfn->close_retries = 0;
        lfn->stats.windows_opened++;
        tr_debug("LFN window open: slot %u ch %"PRIi32" for %"PRIu32" us", lfn->slot, channel, (uint32_t)(window_end - now));
        fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_OPEN, channel);
        fhss_ws_lfn_timer_arm(fhss_structure, (int32_t)(window_end - now));
        return;
    }
    // Close. The MAC defers it while a frame is being received or an ACK
    // sent; ask again shortly, but never past the next window's opening.
    uint32_t next_open = fhss_ws_lfn_open_time(lfn, lfn->slot_start + lfn->own.interval_us);
    if (fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_CLOSE, 0) == 1 &&
            (int32_t)(next_open - (now + cfg->close_retry_us)) > 0) {
        if (!lfn->close_retries++) {
            lfn->stats.closes_deferred++;
        }
        fhss_ws_lfn_timer_arm(fhss_structure, cfg->close_retry_us);
        return;
    }
    lfn->window_open = false;
    if (lfn->resched) {
        // An LTO, an interval change or a sync during the window: the next
        // slot of the new schedule
        lfn->resched = false;
        fhss_ws_lfn_own_next(fhss_structure, now);
        return;
    }
    lfn->slot++;
    lfn->slot_start += lfn->own.interval_us;
    // A timer that fired very late: skip the slots whose window has passed
    while ((int32_t)(fhss_ws_lfn_open_time(lfn, lfn->slot_start) - now) <= 0) {
        lfn->slot++;
        lfn->slot_start += lfn->own.interval_us;
        lfn->stats.windows_skipped++;
    }
    fhss_ws_lfn_timer_arm(fhss_structure, (int32_t)(fhss_ws_lfn_open_time(lfn, lfn->slot_start) - now));
}

static void fhss_ws_lfn_timer_handler(const fhss_api_t *fhss_api, uint16_t delay)
{
    (void) delay;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure) {
        return;
    }
    if (fhss_structure->ws->lfn.listen_running) {
        fhss_ws_lfn_listen_timer(fhss_structure);
    } else if (fhss_structure->ws->lfn.parent_enabled) {
        // A held frame's window is near: the MAC takes it from the queue
        // (and re-arms this poll for the next held frame, if any)
        fhss_structure->ws->lfn.poll_armed = false;
        if (fhss_structure->callbacks.read_tx_queue_size(fhss_structure->fhss_api, false)) {
            fhss_structure->callbacks.tx_poll(fhss_structure->fhss_api);
        }
    }
}

static fhss_lfn_tx_e fhss_ws_lfn_tx_check_callback(const fhss_api_t *api, const uint8_t *destination_address, uint32_t queued_since_us, uint32_t *tx_time)
{
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(api);
    if (!fhss_structure || !fhss_structure->ws->lfn.parent_enabled || fhss_structure->fhss_state != FHSS_SYNCHRONIZED) {
        return FHSS_LFN_TX_NOT_LFN;
    }
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    const fhss_ws_lfn_parent_t *cfg = &lfn->parent_cfg;
    fhss_ws_neighbor_timing_info_t *info = NULL;
    fhss_lfn_sched_t sched;
    uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);

    if (!destination_address) {
        // A frame for the LFN broadcast slots (stage 4)
        if (!lfn->bc_enabled || !fhss_ws_lfn_bc_sched(fhss_structure, now)) {
            return FHSS_LFN_TX_EXPIRED;
        }
        sched = lfn->bc;
    } else {
        info = fhss_ws_lfn_neighbor(fhss_structure, destination_address);
        if (!info || !fhss_ws_lfn_neighbor_sched(fhss_structure, info, &sched)) {
            return FHSS_LFN_TX_NOT_LFN;
        }
    }
    uint64_t ttl_us = (uint64_t)cfg->tx_ttl_intervals * sched.interval_us;
    if (ttl_us > FHSS_LFN_INTERVAL_MAX_US) {
        ttl_us = FHSS_LFN_INTERVAL_MAX_US;
    }
    if ((uint32_t)(now - queued_since_us) > ttl_us) {
        lfn->stats.tx_expired++;
        return FHSS_LFN_TX_EXPIRED;
    }
    // First slot whose TX time (start + offset) leaves at least min_prep
    uint32_t start = fhss_lfn_sched_next(&sched, now + cfg->tx_min_prep_us - cfg->tx_offset_us, NULL);
    uint32_t tx = start + cfg->tx_offset_us;
    if ((int32_t)(tx - now) <= (int32_t)cfg->tx_lead_us) {
        *tx_time = tx;
        if (destination_address) {
            lfn->stats.tx_released++;
            memcpy(lfn->tx_dst, destination_address, 8);
            lfn->tx_dst_time = tx;
            lfn->tx_dst_valid = true;
        } else {
            lfn->stats.bc_tx_released++;
        }
        return FHSS_LFN_TX_NOW;
    }
    // Hold it, and poll the queue tx_lead_us before its TX time unless an
    // earlier poll is armed
    uint32_t poll = tx - cfg->tx_lead_us;
    if (!lfn->poll_armed || (int32_t)(lfn->poll_time - poll) > 0 || (int32_t)(lfn->poll_time - now) < 0) {
        lfn->poll_armed = true;
        lfn->poll_time = poll;
        fhss_ws_lfn_timer_arm(fhss_structure, (int32_t)(poll - now));
    }
    lfn->stats.tx_held++;
    return FHSS_LFN_TX_WAIT;
}

// LFN uplink and the parent's broadcast dwell: does a frame starting at t
// (airtime plus uplink_guard_us) meet the dwell? If so, poll the TX queue at
// the dwell's end (the LFN has no periodic poll).
static bool fhss_ws_lfn_uplink_blocked(fhss_structure_t *fhss_structure, uint32_t t, uint16_t frame_length, uint8_t phy_header_length, uint8_t phy_tail_length)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    uint32_t resume, now, duration;

    if (!lfn->parent_bc_valid) {
        return false;
    }
    // Keep the reference interval start within 2^30 µs of now (32-bit clock)
    now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    if ((int32_t)(now - lfn->parent_bc_start_us) > 0x40000000) {
        uint32_t d = now - lfn->parent_bc_start_us;
        lfn->parent_bc_start_us += d - d % lfn->parent_bc_interval_us;
    }
    duration = fhss_get_tx_time(fhss_structure, frame_length, phy_header_length, phy_tail_length) + lfn->listen_cfg.uplink_guard_us;
    if (!fhss_lfn_dwell_overlap(lfn->parent_bc_start_us, lfn->parent_bc_interval_us, lfn->parent_bc_dwell_us, t, duration, &resume)) {
        return false;
    }
    lfn->stats.uplink_deferred++;
    int32_t delay_us = (int32_t)(resume - now);
    // The event timer counts 50 µs slots
    eventOS_callback_timer_start(fhss_structure->fhss_event_timer, delay_us > 0 ? (uint16_t)(delay_us / 50 + 1) : 1);
    tr_debug("LFN uplink deferred to the end of the parent's broadcast dwell (%"PRIi32" us)", delay_us);
    return true;
}

int fhss_ws_lfn_timing_set(fhss_structure_t *fhss_structure, uint32_t offset_ms, uint32_t interval_ms, uint32_t rx_timestamp_us)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    uint32_t now;

    if (!lfn->listen_enabled) {
        return -1;
    }
    if ((uint64_t)interval_ms * 1000 > FHSS_LFN_INTERVAL_MAX_US || (uint64_t)offset_ms * 1000 > FHSS_LFN_INTERVAL_MAX_US ||
            (interval_ms && lfn->listen_cfg.rx_window_us > interval_ms * 1000u)) {
        return -2;
    }
    if (interval_ms) {
        lfn->listen_cfg.listen_interval_ms = interval_ms;
    }
    if (!lfn->listen_running) {
        // Not started yet: the new interval applies from slot 0
        return 0;
    }
    // The slot whose start is nearest to the reception (the frame came in
    // its window, which may open before the slot start). The interval runs
    // on our clock at the rate measured against the parent's (stage 4).
    fhss_lfn_sched_lto(&lfn->own, rx_timestamp_us + lfn->own.interval_us / 2, offset_ms * 1000u,
                       interval_ms ? fhss_ws_lfn_local_us(lfn, interval_ms * 1000u) : 0);
    tr_info("LFN schedule: offset %"PRIu32" ms, interval %"PRIu32" ms", offset_ms, lfn->listen_cfg.listen_interval_ms);
    if (lfn->window_open) {
        lfn->resched = true;
        return 0;
    }
    now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    fhss_stop_timer(fhss_structure, fhss_ws_lfn_timer_handler);
    fhss_ws_lfn_own_next(fhss_structure, now);
    return 0;
}

int fhss_ws_lfn_neighbor_lto(fhss_structure_t *fhss_structure, const uint8_t eui64[8], uint32_t offset_ms, uint32_t interval_ms, uint32_t tx_timestamp_us)
{
    fhss_ws_neighbor_timing_info_t *info;
    fhss_lfn_sched_t sched;

    if (!fhss_structure->ws->lfn.parent_enabled) {
        return -1;
    }
    info = fhss_ws_lfn_neighbor(fhss_structure, eui64);
    if (!info || !fhss_ws_lfn_neighbor_sched(fhss_structure, info, &sched)) {
        return -1;
    }
    // With an LFN broadcast schedule, the offset given was a phase against
    // the broadcast slots, converted at TX time (fhss_ws_lfn_write_lbt_lto())
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    if (lfn->lto_cache_valid && !memcmp(lfn->lto_eui64, eui64, 8) && lfn->lto_phase_ms == offset_ms) {
        offset_ms = lfn->lto_offset_ms;
        lfn->lto_cache_valid = false;
    }
    // The frame went at its slot start + tx_offset: the slot whose start is
    // nearest, as the LFN takes it (fhss_ws_lfn_timing_set())
    fhss_lfn_sched_lto(&sched, tx_timestamp_us - fhss_structure->ws->lfn.parent_cfg.tx_offset_us + sched.interval_us / 2, offset_ms * 1000u, interval_ms * 1000u);
    info->lfn_uc_timing_info.listen_interval_ms = sched.interval_us / 1000u;
    info->lfn_uc_timing_info.slot_number = sched.anchor_slot;
    info->lfn_uc_timing_info.interval_offset_ms = 0;
    info->lfn_uc_timing_info.lutt_rx_timestamp = sched.anchor_us;
    return 0;
}

int fhss_ws_lfn_listen_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_listen_t *cfg)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    if (!cfg) {
        fhss_ws_lfn_listen_stop(fhss_structure);
        lfn->listen_enabled = false;
        return 0;
    }
    if (!fhss_structure->callbacks.rx_window) {
        return -1;
    }
    if (lfn->parent_enabled) {
        return -3;
    }
    if (!cfg->listen_interval_ms || (uint64_t)cfg->listen_interval_ms * 1000 > FHSS_LFN_INTERVAL_MAX_US ||
            !cfg->rx_window_us || cfg->rx_window_us > cfg->listen_interval_ms * 1000 ||
            cfg->rx_guard_us >= cfg->rx_window_us || !cfg->close_retry_us ||
            cfg->first_slot_delay_us <= cfg->rx_guard_us || cfg->first_slot_delay_us > FHSS_LFN_INTERVAL_MAX_US) {
        return -2;
    }
    fhss_ws_lfn_listen_stop(fhss_structure);
    lfn->listen_cfg = *cfg;
    lfn->listen_enabled = true;
    if (fhss_structure->fhss_state == FHSS_SYNCHRONIZED) {
        fhss_ws_lfn_listen_start(fhss_structure);
    }
    return 0;
}

int fhss_ws_lfn_parent_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_parent_t *cfg)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    if (!cfg) {
        if (lfn->parent_enabled) {
            fhss_stop_timer(fhss_structure, fhss_ws_lfn_timer_handler);
        }
        lfn->parent_enabled = false;
        lfn->poll_armed = false;
        lfn->bc_enabled = false;
        return 0;
    }
    if (lfn->listen_enabled) {
        return -3;
    }
    if (cfg->tx_lead_us > 60000 || cfg->tx_min_prep_us > cfg->tx_lead_us || !cfg->tx_ttl_intervals ||
            cfg->tx_offset_us > FHSS_LFN_INTERVAL_MAX_US / 2) {
        return -2;
    }
    lfn->parent_cfg = *cfg;
    lfn->parent_enabled = true;
    return 0;
}

/* ------------------------------------------------------------------------
 * Wi-SUN FAN 1.1 LFN broadcast schedule and time sync (GS8 stage 4,
 * docs/LFN_MAC.md §2.2 and §3.1).
 *
 * Parent: LFN broadcast slots every interval, numbered from 0 at the start;
 * frames marked LfnBroadcast wait for the next slot. The LBT-IE of every
 * frame gives the slot and offset at TX time; the offset of an LTO-IE to an
 * LFN is converted from a phase against the broadcast slots to an offset
 * against the LFN's slot, as wisun-br-linux's RCP does [S: HIF.md].
 *
 * LFN: the parent's slots on our clock, from each LBT-IE. Each sync corrects
 * the phase, measures the rate of our clock against the parent's (from a
 * reference sync up to 2^30 µs back) and moves our own unicast slots by the
 * same correction, with their interval converted to our clock: our slots
 * stay where the parent expects them (its view comes from our last LUTT-IE
 * and its own clock). Windows widen by drift_ppm per second since the last
 * sync [M: Silicon Labs' LFN widens about 0.11 ms per second].
 * ------------------------------------------------------------------------ */

static uint32_t fhss_ws_lfn_local_us(const struct fhss_ws_lfn *lfn, uint32_t parent_us)
{
    return (uint32_t)((int64_t)parent_us + (int64_t)parent_us * lfn->rate_ppb / 1000000000);
}

static uint32_t fhss_ws_lfn_widening(struct fhss_ws_lfn *lfn, uint32_t t)
{
    uint32_t since;
    uint64_t w;

    if (!lfn->pbc_valid || !lfn->listen_cfg.drift_ppm) {
        return 0;
    }
    since = t - lfn->sync_us;
    if ((int32_t)since < 0) {
        return 0;
    }
    if (since > 0x40000000u) {
        return lfn->listen_cfg.widening_max_us;
    }
    w = (uint64_t)since * lfn->listen_cfg.drift_ppm / 1000000u;
    return w > lfn->listen_cfg.widening_max_us ? lfn->listen_cfg.widening_max_us : (uint32_t)w;
}

static int32_t fhss_ws_lfn_bc_channel(fhss_structure_t *fhss_structure, uint16_t slot, uint16_t bsi, uint8_t function, uint16_t fixed)
{
    int32_t channel = fixed;
    const fhss_ws_configuration_t *cfg = &fhss_structure->ws->fhss_configuration;

    if (!fhss_structure->number_of_bc_channels) {
        return function == WS_FIXED_CHANNEL ? channel : -1;
    }
    if (function == WS_TR51CF) {
        channel = tr51_get_bc_channel_index(fhss_structure->ws->tr51_channel_table, fhss_structure->ws->tr51_output_table,
                                            slot % fhss_structure->number_of_bc_channels, bsi, fhss_structure->number_of_bc_channels, NULL);
        channel = fhss_channel_index_from_mask(cfg->channel_mask, channel, fhss_structure->number_of_channels);
    } else if (function == WS_DH1CF) {
        channel = dh1cf_get_bc_channel_index(slot, bsi, fhss_structure->number_of_bc_channels);
        channel = fhss_channel_index_from_mask(cfg->channel_mask, channel, fhss_structure->number_of_channels);
    } else if (function == WS_VENDOR_DEF_CF) {
        if (!cfg->vendor_defined_cf) {
            return -1;
        }
        channel = cfg->vendor_defined_cf(fhss_structure->fhss_api, slot, NULL, bsi, fhss_structure->number_of_channels);
    }
    return channel;
}

// Parent: keep the broadcast schedule's anchor within 2^30 µs of t
static bool fhss_ws_lfn_bc_sched(fhss_structure_t *fhss_structure, uint32_t t)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    if (!lfn->bc_enabled) {
        return false;
    }
    if ((int32_t)(t - lfn->bc.anchor_us) > 0x40000000) {
        fhss_lfn_sched_reanchor(&lfn->bc, t);
    }
    return true;
}

// Parent, at TX time: LBT-IE (slot, offset in ms) and the LTO-IE offset
static void fhss_ws_lfn_write_lbt_lto(fhss_structure_t *fhss_structure, uint8_t *ptr, uint8_t length, uint32_t tx_time)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    struct ws_ie_t header_ie;
    uint32_t offset_us;

    if (!fhss_ws_lfn_bc_sched(fhss_structure, tx_time)) {
        return;
    }
    header_ie.id = WH_IE_ID;
    if (fhss_ws_ie_header_discover(ptr, length, &header_ie, WH_SUB_ID_LBT) >= 5) {
        uint16_t slot = fhss_lfn_sched_slot_at(&lfn->bc, tx_time, &offset_us);
        uint32_t offset_ms = (offset_us + 500) / 1000;
        if (offset_ms >= lfn->bc_cfg.interval_ms) {
            offset_ms = lfn->bc_cfg.interval_ms - 1;
        }
        common_write_16_bit_inverse(slot, header_ie.content_ptr);
        common_write_24_bit_inverse(offset_ms, header_ie.content_ptr + 2);
    }
    // LTO-IE to an LFN: the LLC wrote the phase it wants against our
    // broadcast slots; the LFN applies the offset from its slot nearest to
    // the frame (docs/LFN_MAC.md §2.1). The frame being built is the one
    // lfn_tx_check released last, for this TX time.
    if (fhss_ws_ie_header_discover(ptr, length, &header_ie, WH_SUB_ID_LTO) >= 6 && lfn->tx_dst_valid && lfn->tx_dst_time == tx_time) {
        fhss_ws_neighbor_timing_info_t *info = fhss_ws_lfn_neighbor(fhss_structure, lfn->tx_dst);
        fhss_lfn_sched_t sched;
        uint32_t phase_ms = common_read_24_bit_inverse(header_ie.content_ptr);
        uint32_t interval_ms = common_read_24_bit_inverse(header_ie.content_ptr + 3);
        if (info && fhss_ws_lfn_neighbor_sched(fhss_structure, info, &sched) && interval_ms) {
            uint32_t s_n = fhss_lfn_sched_next(&sched, tx_time - lfn->parent_cfg.tx_offset_us - sched.interval_us / 2, NULL);
            uint32_t b_start = fhss_lfn_sched_next(&lfn->bc, s_n, NULL);
            uint32_t period_ms = interval_ms < lfn->bc_cfg.interval_ms ? interval_ms : lfn->bc_cfg.interval_ms;
            uint32_t target = b_start + phase_ms * 1000u;
            uint32_t offset_ms = ((target - s_n) / 1000u) % period_ms;
            common_write_24_bit_inverse(offset_ms, header_ie.content_ptr);
            memcpy(lfn->lto_eui64, lfn->tx_dst, 8);
            lfn->lto_cache_valid = true;
            lfn->lto_phase_ms = phase_ms;
            lfn->lto_offset_ms = offset_ms;
        }
    }
}

static void fhss_ws_lfn_bc_arm_next(fhss_structure_t *fhss_structure, uint32_t now)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    lfn->bc_slot_start = fhss_lfn_sched_next(&lfn->pbc, now, &lfn->bc_slot);
    for (;;) {
        bool listen = !lfn->listen_cfg.bc_sync_only || !lfn->pbc_sync_period || !(lfn->bc_slot % lfn->pbc_sync_period);
        if (listen && (int32_t)(fhss_ws_lfn_open_time(lfn, lfn->bc_slot_start) - now) > 0) {
            break;
        }
        lfn->bc_slot++;
        lfn->bc_slot_start += lfn->pbc.interval_us;
    }
    fhss_start_timer(fhss_structure, (uint32_t)(fhss_ws_lfn_open_time(lfn, lfn->bc_slot_start) - now), fhss_ws_lfn_bc_timer_handler);
}

static void fhss_ws_lfn_bc_timer_handler(const fhss_api_t *fhss_api, uint16_t delay)
{
    (void) delay;
    fhss_structure_t *fhss_structure = fhss_get_object_with_api(fhss_api);
    if (!fhss_structure || !fhss_structure->ws->lfn.listen_running || !fhss_structure->ws->lfn.pbc_valid) {
        return;
    }
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    const fhss_ws_lfn_listen_t *cfg = &lfn->listen_cfg;
    uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
    uint32_t widening = fhss_ws_lfn_widening(lfn, lfn->bc_slot_start);

    if (!lfn->bc_window_open) {
        if (lfn->window_open) {
            // The unicast window holds the radio: skip this broadcast slot
            lfn->stats.bc_windows_skipped++;
            fhss_ws_lfn_bc_arm_next(fhss_structure, now);
            return;
        }
        // Re-anchor at this slot (32-bit clock)
        lfn->pbc.anchor_us = lfn->bc_slot_start;
        lfn->pbc.anchor_slot = lfn->bc_slot;
        int32_t channel = fhss_ws_lfn_bc_channel(fhss_structure, lfn->bc_slot, lfn->pbc_bsi, lfn->pbc_channel_function, lfn->pbc_fixed_channel);
        if (channel < 0) {
            lfn->stats.bc_windows_skipped++;
            fhss_ws_lfn_bc_arm_next(fhss_structure, now);
            return;
        }
        fhss_structure->rx_channel = channel;
        lfn->bc_window_open = true;
        lfn->bc_close_retries = 0;
        lfn->stats.bc_windows_opened++;
        uint32_t end = lfn->bc_slot_start - cfg->rx_guard_us + cfg->bc_rx_window_us + widening;
        tr_debug("LFN broadcast window open: slot %u ch %"PRIi32" for %"PRIu32" us (widening %"PRIu32" us)",
                 lfn->bc_slot, channel, (uint32_t)(end - now), widening);
        fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_OPEN, channel);
        fhss_start_timer(fhss_structure, (int32_t)(end - now) > 0 ? end - now : 1, fhss_ws_lfn_bc_timer_handler);
        return;
    }
    uint32_t next_open = fhss_ws_lfn_open_time(lfn, lfn->bc_slot_start + lfn->pbc.interval_us);
    if (fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_CLOSE, 0) == 1 &&
            (int32_t)(next_open - (now + cfg->close_retry_us)) > 0) {
        if (!lfn->bc_close_retries++) {
            lfn->stats.closes_deferred++;
        }
        fhss_start_timer(fhss_structure, cfg->close_retry_us, fhss_ws_lfn_bc_timer_handler);
        return;
    }
    lfn->bc_window_open = false;
    if (lfn->resched_after_bc) {
        lfn->resched_after_bc = false;
        fhss_stop_timer(fhss_structure, fhss_ws_lfn_timer_handler);
        fhss_ws_lfn_own_next(fhss_structure, now);
    }
    fhss_ws_lfn_bc_arm_next(fhss_structure, now);
}

int fhss_ws_lfn_bc_set(fhss_structure_t *fhss_structure, const fhss_ws_lfn_bc_t *cfg)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    if (!cfg) {
        lfn->bc_enabled = false;
        return 0;
    }
    if (!lfn->parent_enabled) {
        return -1;
    }
    if (!cfg->interval_ms || (uint64_t)cfg->interval_ms * 1000 > FHSS_LFN_INTERVAL_MAX_US) {
        return -2;
    }
    lfn->bc_cfg = *cfg;
    lfn->bc.interval_us = cfg->interval_ms * 1000;
    lfn->bc.anchor_us = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api) + lfn->bc.interval_us;
    lfn->bc.anchor_slot = 0;
    lfn->bc_enabled = true;
    tr_info("LFN broadcast schedule: interval %"PRIu32" ms, BSI %u", cfg->interval_ms, cfg->bsi);
    return 0;
}

int fhss_ws_lfn_bc_next(fhss_structure_t *fhss_structure, uint16_t *slot, uint32_t *start_us)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);

    if (!fhss_ws_lfn_bc_sched(fhss_structure, now)) {
        return -1;
    }
    uint32_t start = fhss_lfn_sched_next(&lfn->bc, now + lfn->parent_cfg.tx_min_prep_us - lfn->parent_cfg.tx_offset_us, slot);
    if (start_us) {
        *start_us = start;
    }
    return 0;
}

// Move our own unicast slots by delta_us (a sync's phase correction), and
// their interval to our clock at the current rate
static void fhss_ws_lfn_own_follow(fhss_structure_t *fhss_structure, int32_t delta_us)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    lfn->own.anchor_us += (uint32_t)delta_us;
    lfn->own.interval_us = fhss_ws_lfn_local_us(lfn, lfn->listen_cfg.listen_interval_ms * 1000u);
    if (lfn->window_open) {
        lfn->resched = true;
    } else if (lfn->bc_window_open) {
        lfn->resched_after_bc = true;
    } else {
        uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
        fhss_stop_timer(fhss_structure, fhss_ws_lfn_timer_handler);
        fhss_ws_lfn_own_next(fhss_structure, now);
    }
}

int fhss_ws_lfn_bc_sync(fhss_structure_t *fhss_structure, const fhss_ws_lfn_bc_sync_t *sync)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;
    uint32_t nominal_us, start;
    int32_t delta = 0;
    bool same;

    if (!lfn->listen_enabled) {
        return -1;
    }
    if (!sync->interval_ms || (uint64_t)sync->interval_ms * 1000 > FHSS_LFN_INTERVAL_MAX_US || sync->offset_ms >= sync->interval_ms) {
        return -2;
    }
    nominal_us = sync->interval_ms * 1000;
    // The start of the frame's slot, on our clock
    start = sync->rx_timestamp_us - sync->offset_ms * 1000;
    same = lfn->pbc_valid && lfn->pbc_nominal_us == nominal_us && lfn->pbc_bsi == sync->bsi;
    // The LBT-IE offset is in ms: a frame sent within a slot (LPA, unicast
    // LPC) gives its start to ±0.5 ms only, a frame sent at the slot start
    // (offset 0: LTS, broadcast LPC) to the TX timing. Once synced, only the
    // latter correct the phase and measure the rate.
    if (same && sync->offset_ms) {
        return 0;
    }
    if (same) {
        int16_t dn = (int16_t)(sync->slot - lfn->pbc.anchor_slot);
        uint32_t predicted = lfn->pbc.anchor_us + (uint32_t)((int64_t)dn * lfn->pbc.interval_us);
        delta = (int32_t)(start - predicted);
        if (delta > (int32_t)(nominal_us / 4) || delta < -(int32_t)(nominal_us / 4)) {
            same = false;
            delta = 0;
        }
    }
    if (!same) {
        // A new schedule (or a new parent): the rate is measured again
        lfn->rate_ref_valid = false;
        lfn->rate_ppb = 0;
    } else if (lfn->rate_ref_valid) {
        uint16_t dn = sync->slot - lfn->rate_ref_slot;
        uint32_t dt = start - lfn->rate_ref_us;
        if (dt > 0x40000000u || (int16_t)dn <= 0) {
            // Too old for the 32-bit clock: this sync is the new reference,
            // the rate is kept
            lfn->rate_ref_valid = false;
        } else if (dn >= (lfn->pbc_sync_period ? lfn->pbc_sync_period : 1)) {
            int64_t total = (int64_t)dn * nominal_us;
            int64_t r = ((int64_t)dt - total) * 1000000000 / total;
            if (r > -500000 && r < 500000) {
                lfn->rate_ppb = (int32_t)r;
            }
        }
    }
    if (!lfn->rate_ref_valid && !sync->offset_ms) {
        lfn->rate_ref_valid = true;
        lfn->rate_ref_us = start;
        lfn->rate_ref_slot = sync->slot;
    }
    lfn->pbc_nominal_us = nominal_us;
    lfn->pbc_bsi = sync->bsi;
    lfn->pbc_sync_period = sync->sync_period;
    lfn->pbc_channel_function = sync->channel_function;
    lfn->pbc_fixed_channel = sync->fixed_channel;
    lfn->pbc.anchor_us = start;
    lfn->pbc.anchor_slot = sync->slot;
    lfn->pbc.interval_us = fhss_ws_lfn_local_us(lfn, nominal_us);
    lfn->sync_us = start;
    lfn->stats.bc_syncs++;
    lfn->stats.rate_ppb = lfn->rate_ppb;
    lfn->stats.last_sync_error_us = delta;
    tr_debug("LFN sync: slot %u, error %"PRIi32" us, rate %"PRIi32" ppb", sync->slot, delta, lfn->rate_ppb);
    // Our unicast slots keep their place in the parent's time base
    fhss_ws_lfn_own_follow(fhss_structure, same ? delta : 0);
    if (!lfn->bc_window_open) {
        uint32_t now = fhss_structure->callbacks.read_timestamp(fhss_structure->fhss_api);
        lfn->pbc_valid = true;
        fhss_stop_timer(fhss_structure, fhss_ws_lfn_bc_timer_handler);
        fhss_ws_lfn_bc_arm_next(fhss_structure, now);
    }
    lfn->pbc_valid = true;
    return 0;
}

int fhss_ws_lfn_bc_stop(fhss_structure_t *fhss_structure)
{
    struct fhss_ws_lfn *lfn = &fhss_structure->ws->lfn;

    fhss_stop_timer(fhss_structure, fhss_ws_lfn_bc_timer_handler);
    if (lfn->bc_window_open) {
        fhss_structure->callbacks.rx_window(fhss_structure->fhss_api, FHSS_RX_WINDOW_CLOSE, 0);
        lfn->bc_window_open = false;
    }
    lfn->pbc_valid = false;
    lfn->rate_ref_valid = false;
    lfn->rate_ppb = 0;
    lfn->resched_after_bc = false;
    if (lfn->listen_running) {
        // Our own slots back on our clock's interval
        lfn->own.interval_us = lfn->listen_cfg.listen_interval_ms * 1000u;
    }
    return 0;
}
