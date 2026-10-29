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

#include <string.h>
#include "nsconfig.h"
#include <stdint.h>
#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/ns_list.h"
#include <stdlib.h>
#include "nanostack/mac/fhss_config.h"
#include "nanostack/ws_management_api.h"
#include "nanostack/mac/fhss_ws_extension.h"

#include "nwk_interface/protocol.h"
#include "6lowpan/ws/ws_common.h"
#include "6lowpan/ws/ws_bootstrap.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "6lowpan/ws/ws_bootstrap_6lbr.h"
#include "6lowpan/ws/ws_cfg_settings.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/mac/mac_helper.h"
#include "6lowpan/ws/ws_pae_controller.h"

#define TRACE_GROUP "wsmg"


int ws_management_node_init(
    int8_t interface_id,
    uint8_t regulatory_domain,
    char *network_name_ptr,
    fhss_timer_t *fhss_timer_ptr)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    if (!network_name_ptr || !fhss_timer_ptr) {
        return -2;
    }

    ws_phy_cfg_t phy_cfg;
    if (ws_cfg_phy_get(&phy_cfg) < 0) {
        return -3;
    }

    phy_cfg.regulatory_domain = regulatory_domain;

    if (ws_cfg_phy_set(cur, &phy_cfg, 0) < 0) {
        return -4;
    }

    ws_gen_cfg_t gen_cfg;
    if (ws_cfg_gen_get(&gen_cfg) < 0) {
        return -3;
    }

    strncpy(gen_cfg.network_name, network_name_ptr, 32);

    if (ws_cfg_gen_set(cur, &gen_cfg, 0) < 0) {
        return -4;
    }

    if (cur && ws_info(cur)) {
        cur->ws_info->fhss_timer_ptr = fhss_timer_ptr;
    }

    return 0;
}

int ws_management_network_name_set(
    int8_t interface_id,
    char *network_name_ptr)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!network_name_ptr) {
        return -2;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_gen_get(&cfg) < 0) {
        return -3;
    }

    strncpy(cfg.network_name, network_name_ptr, 32);

    if (ws_cfg_gen_set(cur, &cfg, 0) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_network_name_get(
    int8_t interface_id,
    char *network_name_ptr)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!network_name_ptr) {
        return -2;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_gen_get(&cfg) < 0) {
        return -3;
    }

    memcpy(network_name_ptr, cfg.network_name, 32);

    return 0;
}

int ws_management_network_name_validate(
    int8_t interface_id,
    char *network_name_ptr)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!network_name_ptr) {
        return -2;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_gen_get(&cfg) < 0) {
        return -3;
    }

    strncpy(cfg.network_name, network_name_ptr, 32);

    if (ws_cfg_gen_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_domain_configuration_set(
    int8_t interface_id,
    uint8_t regulatory_domain,
    uint8_t phy_mode_id,
    uint8_t channel_plan_id)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_phy_cfg_t cfg;
    ws_phy_cfg_t cfg_default;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -3;
    }

    if (ws_cfg_phy_default_set(&cfg_default) < 0) {
        return -3;
    }

    if (regulatory_domain == 255) {
        cfg.regulatory_domain = cfg_default.regulatory_domain;
    } else if (regulatory_domain != 0) {
        cfg.regulatory_domain = regulatory_domain;
    }

    if (phy_mode_id == 255) {
        cfg.phy_mode_id = cfg_default.phy_mode_id;
    } else if (phy_mode_id != 0) {
        cfg.phy_mode_id = phy_mode_id;
    }

    if (channel_plan_id == 255) {
        cfg.channel_plan_id = cfg_default.channel_plan_id;
    } else if (channel_plan_id != 0) {
        cfg.channel_plan_id = channel_plan_id;
    }

    if (ws_cfg_phy_set(cur, &cfg, 0) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_domain_configuration_get(
    int8_t interface_id,
    uint8_t *regulatory_domain,
    uint8_t *phy_mode_id,
    uint8_t *channel_plan_id)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_phy_cfg_t cfg;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -2;
    }

    if (regulatory_domain) {
        *regulatory_domain = cfg.regulatory_domain;
    }
    if (phy_mode_id) {
        *phy_mode_id = cfg.phy_mode_id;
    }
    if (channel_plan_id) {
        *channel_plan_id = cfg.channel_plan_id;
    }

    return 0;
}

int ws_management_domain_configuration_validate(
    int8_t interface_id,
    uint8_t regulatory_domain,
    uint8_t phy_mode_id,
    uint8_t channel_plan_id)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_phy_cfg_t cfg;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -3;
    }

    cfg.regulatory_domain = regulatory_domain;
    cfg.phy_mode_id = phy_mode_id;
    cfg.channel_plan_id = channel_plan_id;

    if (ws_cfg_phy_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

// If using PhyModeId and ChanPlanId on FAN1.0 API, convert operating mode and operating class
// into PhyModeId ChanPlanId
void ws_management_convert_legacy_config(ws_phy_cfg_t *cfg)
{
    if (cfg->operating_class & OPERATING_CLASS_CHAN_PLAN_ID_BIT) {
        // Channel plan ID is coded on the operating class LSB
        cfg->channel_plan_id = cfg->operating_class & OPERATING_CLASS_CHAN_PLAN_ID_MASK;
    }

    if (cfg->operating_mode & OPERATING_MODE_PHY_MODE_ID_BIT) {
        cfg->phy_mode_id = cfg->operating_mode & OPERATING_MODE_PHY_MODE_ID_MASK;
    }
}

int ws_management_regulatory_domain_set(
    int8_t interface_id,
    uint8_t regulatory_domain,
    uint8_t operating_class,
    uint8_t operating_mode)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_phy_cfg_t cfg;
    ws_phy_cfg_t cfg_default;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -3;
    }

    if (ws_cfg_phy_default_set(&cfg_default) < 0) {
        return -3;
    }

    if (regulatory_domain != 255) {
        cfg.regulatory_domain = regulatory_domain;
    } else {
        cfg.regulatory_domain = cfg_default.regulatory_domain;
    }
    if (operating_mode != 255) {
        cfg.operating_mode = operating_mode;
    } else {
        cfg.operating_mode = cfg_default.operating_mode;
    }
    if (operating_class != 255) {
        cfg.operating_class = operating_class;
    } else {
        cfg.operating_class = cfg_default.operating_class;
    }

    ws_management_convert_legacy_config(&cfg);
    if (ws_cfg_phy_set(cur, &cfg, 0) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_regulatory_domain_get(
    int8_t interface_id,
    uint8_t *regulatory_domain,
    uint8_t *operating_class,
    uint8_t *operating_mode)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!regulatory_domain || !operating_class || !operating_mode) {
        return -2;
    }

    ws_phy_cfg_t cfg;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -3;
    }

    *regulatory_domain = cfg.regulatory_domain;
    *operating_class = cfg.operating_class;
    *operating_mode = cfg.operating_mode;

    return 0;
}

int ws_management_regulatory_domain_validate(
    int8_t interface_id,
    uint8_t regulatory_domain,
    uint8_t operating_class,
    uint8_t operating_mode)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_phy_cfg_t cfg;
    if (ws_cfg_phy_get(&cfg) < 0) {
        return -3;
    }

    cfg.regulatory_domain = regulatory_domain;
    cfg.operating_class = operating_class;
    cfg.operating_mode = operating_mode;

    if (ws_cfg_phy_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_network_size_set(
    int8_t interface_id,
    uint8_t network_size)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_network_size_get(&cfg) < 0) {
        return -3;
    }

    cfg.network_size = network_size;

    if (ws_cfg_network_size_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_network_size_get(
    int8_t interface_id,
    uint8_t *network_size)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!network_size) {
        return -2;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_network_size_get(&cfg) < 0) {
        return -3;
    }

    *network_size = cfg.network_size;

    return 0;
}

int ws_management_network_size_validate(
    int8_t interface_id,
    uint8_t network_size)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_gen_cfg_t cfg;
    if (ws_cfg_network_size_get(&cfg) < 0) {
        return -3;
    }

    cfg.network_size = network_size;

    if (ws_cfg_network_size_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_channel_mask_set(
    int8_t interface_id,
    uint32_t channel_mask[8])
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    ws_fhss_cfg_t cfg_default;
    if (ws_cfg_fhss_default_set(&cfg_default) < 0) {
        return -2;
    }

    if (channel_mask) {
        memcpy(cfg.fhss_channel_mask, channel_mask, sizeof(uint32_t) * 8);
    } else {
        // Use the default
        memcpy(cfg.fhss_channel_mask, cfg_default.fhss_channel_mask, sizeof(uint32_t) * 8);
    }


    if (ws_cfg_fhss_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_channel_mask_get(
    int8_t interface_id,
    uint32_t *channel_mask)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!channel_mask) {
        return -2;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    memcpy(channel_mask, cfg.fhss_channel_mask, sizeof(uint32_t) * 8);

    return 0;
}

int ws_management_channel_mask_validate(
    int8_t interface_id,
    uint32_t channel_mask[8])
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    memcpy(cfg.fhss_channel_mask, channel_mask, sizeof(uint32_t) * 8);

    if (ws_cfg_fhss_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_channel_plan_set(
    int8_t interface_id,
    uint8_t channel_plan,
    uint8_t uc_channel_function,
    uint8_t bc_channel_function,
    uint32_t ch0_freq, // Stack can not modify this
    uint8_t channel_spacing,// Stack can not modify this
    uint8_t number_of_channels)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !ws_info(cur)) {
        return -1;
    }
    cur->ws_info->hopping_schedule.channel_plan = channel_plan;
    cur->ws_info->hopping_schedule.uc_channel_function = uc_channel_function;
    cur->ws_info->hopping_schedule.bc_channel_function = bc_channel_function;
    cur->ws_info->hopping_schedule.ch0_freq = ch0_freq;
    cur->ws_info->hopping_schedule.channel_spacing = channel_spacing;
    cur->ws_info->hopping_schedule.number_of_channels = number_of_channels;

    // TODO update fields to llc
    return 0;
}

int ws_management_fhss_timing_configure(
    int8_t interface_id,
    uint8_t fhss_uc_dwell_interval,
    uint32_t fhss_broadcast_interval,
    uint8_t fhss_bc_dwell_interval)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    ws_fhss_cfg_t cfg_default;
    if (ws_cfg_fhss_default_set(&cfg_default) < 0) {
        return -2;
    }

    if (fhss_uc_dwell_interval == 0) {
        cfg.fhss_uc_dwell_interval = cfg_default.fhss_uc_dwell_interval;
    } else {
        cfg.fhss_uc_dwell_interval = fhss_uc_dwell_interval;
    }

    if (fhss_broadcast_interval > 0xffffff) {
        cfg.fhss_bc_interval = cfg_default.fhss_bc_interval;
    } else if (fhss_broadcast_interval > 0) {
        cfg.fhss_bc_interval = fhss_broadcast_interval;
    }

    if (fhss_bc_dwell_interval == 0) {
        cfg.fhss_bc_dwell_interval = cfg_default.fhss_bc_dwell_interval;
    } else {
        cfg.fhss_bc_dwell_interval = fhss_bc_dwell_interval;
    }

    if (ws_cfg_fhss_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_fhss_unicast_channel_function_configure(
    int8_t interface_id,
    uint8_t channel_function,
    uint16_t fixed_channel,
    uint8_t dwell_interval)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    ws_fhss_cfg_t cfg_default;
    if (ws_cfg_fhss_default_set(&cfg_default) < 0) {
        return -2;
    }

    if (dwell_interval == 0) {
        cfg.fhss_uc_dwell_interval = cfg_default.fhss_uc_dwell_interval;
    } else {
        cfg.fhss_uc_dwell_interval = dwell_interval;
    }
    if (channel_function < 0xff) {
        cfg.fhss_uc_channel_function = channel_function;
    } else {
        cfg.fhss_uc_channel_function = cfg_default.fhss_uc_channel_function;
    }

    if (fixed_channel < 0xffff) {
        cfg.fhss_uc_fixed_channel = fixed_channel;
    } else {
        cfg.fhss_uc_fixed_channel = cfg_default.fhss_uc_fixed_channel;
    }

    if (ws_cfg_fhss_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_fhss_unicast_channel_function_get(
    int8_t interface_id,
    uint8_t *channel_function,
    uint16_t *fixed_channel,
    uint8_t *dwell_interval)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!channel_function || !fixed_channel || !dwell_interval) {
        return -2;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    *dwell_interval = cfg.fhss_uc_dwell_interval;
    *channel_function = cfg.fhss_uc_channel_function;
    *fixed_channel = cfg.fhss_uc_fixed_channel;

    return 0;
}

int ws_management_fhss_unicast_channel_function_validate(
    int8_t interface_id,
    uint8_t channel_function,
    uint16_t fixed_channel,
    uint8_t dwell_interval)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    cfg.fhss_uc_dwell_interval = dwell_interval;
    cfg.fhss_uc_channel_function = channel_function;
    cfg.fhss_uc_fixed_channel = fixed_channel;

    if (ws_cfg_fhss_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_fhss_broadcast_channel_function_configure(
    int8_t interface_id,
    uint8_t channel_function,
    uint16_t fixed_channel,
    uint8_t dwell_interval,
    uint32_t broadcast_interval)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }
    ws_fhss_cfg_t cfg_default;
    if (ws_cfg_fhss_default_set(&cfg_default) < 0) {
        return -2;
    }

    if (dwell_interval == 0) {
        cfg.fhss_bc_dwell_interval = cfg_default.fhss_bc_dwell_interval;
    } else {
        cfg.fhss_bc_dwell_interval = dwell_interval;
    }

    if (broadcast_interval > 0xffffff) {
        cfg.fhss_bc_interval = cfg_default.fhss_bc_interval;
    } else if (broadcast_interval > 0) {
        cfg.fhss_bc_interval = broadcast_interval;
    }

    if (channel_function != 0xff) {
        cfg.fhss_bc_channel_function = channel_function;
    } else {
        cfg.fhss_bc_channel_function = cfg_default.fhss_bc_channel_function;
    }

    if (fixed_channel != 0xffff) {
        cfg.fhss_bc_fixed_channel = fixed_channel;
    } else {
        cfg.fhss_bc_fixed_channel = cfg_default.fhss_bc_fixed_channel;
    }

    if (ws_cfg_fhss_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_fhss_broadcast_channel_function_get(
    int8_t interface_id,
    uint8_t *channel_function,
    uint16_t *fixed_channel,
    uint8_t *dwell_interval,
    uint32_t *broadcast_interval)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!channel_function || !fixed_channel || !dwell_interval) {
        return -2;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    *dwell_interval = cfg.fhss_bc_dwell_interval;
    *broadcast_interval = cfg.fhss_bc_interval;
    *channel_function = cfg.fhss_bc_channel_function;
    *fixed_channel = cfg.fhss_bc_fixed_channel;

    return 0;
}

int ws_management_fhss_broadcast_channel_function_validate(
    int8_t interface_id,
    uint8_t channel_function,
    uint16_t fixed_channel,
    uint8_t dwell_interval,
    uint32_t broadcast_interval)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_fhss_cfg_t cfg;
    if (ws_cfg_fhss_get(&cfg) < 0) {
        return -2;
    }

    cfg.fhss_bc_dwell_interval = dwell_interval;
    cfg.fhss_bc_interval = broadcast_interval;
    cfg.fhss_bc_channel_function = channel_function;
    cfg.fhss_bc_fixed_channel = fixed_channel;

    if (ws_cfg_fhss_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_management_timing_parameters_set(
    int8_t interface_id,
    uint16_t disc_trickle_imin,
    uint16_t disc_trickle_imax,
    uint8_t disc_trickle_k,
    uint16_t pan_timeout)
{
    protocol_interface_info_entry_t *cur;

    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_timing_cfg_t cfg;
    if (ws_cfg_timing_get(&cfg) < 0) {
        return -2;
    }

    ws_timing_cfg_t cfg_default;
    if (ws_cfg_timing_default_set(&cfg_default) < 0) {
        return -2;
    }

    if (disc_trickle_imin > 0) {
        cfg.disc_trickle_imin = disc_trickle_imin;
    } else {
        cfg.disc_trickle_imin = cfg_default.disc_trickle_imin;
    }

    if (disc_trickle_imax > 0) {
        cfg.disc_trickle_imax = disc_trickle_imax;
    } else {
        cfg.disc_trickle_imax = cfg_default.disc_trickle_imax;;
    }

    if (disc_trickle_k > 0) {
        cfg.disc_trickle_k = disc_trickle_k;
    } else {
        cfg.disc_trickle_k = cfg_default.disc_trickle_k;;
    }

    if (pan_timeout > 0) {
        cfg.pan_timeout = pan_timeout;
    } else {
        cfg.pan_timeout = cfg_default.pan_timeout;;
    }

    if (ws_cfg_timing_set(cur, &cfg, 0) < 0) {
        return -3;
    }

    return 0;
}

int ws_management_timing_parameters_get(
    int8_t interface_id,
    uint16_t *disc_trickle_imin,
    uint16_t *disc_trickle_imax,
    uint8_t *disc_trickle_k,
    uint16_t *pan_timeout)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }
    if (!disc_trickle_imin || !disc_trickle_imax || !disc_trickle_k || !pan_timeout) {
        return -2;
    }

    ws_timing_cfg_t cfg;
    if (ws_cfg_timing_get(&cfg) < 0) {
        return -2;
    }

    *disc_trickle_imin = cfg.disc_trickle_imin;
    *disc_trickle_imax = cfg.disc_trickle_imax;
    *disc_trickle_k = cfg.disc_trickle_k;
    *pan_timeout = cfg.pan_timeout;

    return 0;
}

int ws_management_timing_parameters_validate(
    int8_t interface_id,
    uint16_t disc_trickle_imin,
    uint16_t disc_trickle_imax,
    uint8_t disc_trickle_k,
    uint16_t pan_timeout)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (interface_id >= 0 && (!cur || !ws_info(cur))) {
        return -1;
    }

    ws_timing_cfg_t cfg;
    if (ws_cfg_timing_get(&cfg) < 0) {
        return -2;
    }

    cfg.disc_trickle_imin = disc_trickle_imin;
    cfg.disc_trickle_imax = disc_trickle_imax;
    cfg.disc_trickle_k = disc_trickle_k;
    cfg.pan_timeout = pan_timeout;

    if (ws_cfg_timing_validate(&cfg) < 0) {
        return -4;
    }

    return 0;
}

int ws_stack_info_get(int8_t interface_id, ws_stack_info_t *info_ptr)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !ws_info(cur) || !info_ptr) {
        return -1;
    }
    return ws_bootstrap_stack_info_get(cur, info_ptr);
}

int ws_neighbor_info_get(
    int8_t interface_id,
    ws_neighbour_info_t *neighbor_ptr,
    uint16_t count)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !ws_info(cur)) {
        return -1;
    }
    return ws_bootstrap_neighbor_info_get(cur, neighbor_ptr, count);
}

int ws_device_min_sens_set(
    int8_t interface_id,
    uint8_t device_min_sens)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !ws_info(cur)) {
        return -1;
    }
    DEVICE_MIN_SENS = device_min_sens;
    ws_info(cur)->device_min_sens = device_min_sens;
    return 0;
}

int ws_management_phy_capability_set(
    int8_t interface_id,
    ws_management_pcap_info_t *pcap_list)
{
#ifdef HAVE_WS_VERSION_1_1
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !ws_info(cur) || pcap_list->length_of_list > 7) {
        return -1;
    }

    //Set supported configure for MDR
    //TODO add validation for Phy_type and operation modes
    cur->ws_info->phy_cap_info.length_of_list = pcap_list->length_of_list;
    for (int i = 0; i < pcap_list->length_of_list; i++) {
        cur->ws_info->phy_cap_info.pcap[i].phy_type = pcap_list->pcap[i].phy_type;
        cur->ws_info->phy_cap_info.pcap[i].operating_mode = pcap_list->pcap[i].operating_mode;
    }

    return 0;

#else
    (void)interface_id;
    (void)pcap_list;
    return -1;
#endif
}



int ws_management_fan_version_set(int8_t interface_id, uint8_t version)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur)) {
        return -1;
    }
    if (version != WS_FAN_VERSION_1_0 && version != WS_FAN_VERSION_1_1) {
        return -2;
    }
#ifndef HAVE_WS_VERSION_1_1
    if (version != WS_FAN_VERSION_1_0) {
        return -2;
    }
#endif
    // As ws_test_version_set(): the default for interfaces created later too
    test_pan_version = version;
    cur->ws_info->version = version;
    cur->ws_info->pan_information.version = ws_version_1_1(cur) ? WS_FAN_VERSION_1_1 : WS_FAN_VERSION_1_0;
    return 0;
}

int ws_management_fan_version_get(int8_t interface_id, uint8_t *version)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur) || !version) {
        return -1;
    }
    *version = ws_version_1_1(cur) ? WS_FAN_VERSION_1_1 : WS_FAN_VERSION_1_0;
    return 0;
}

int ws_management_rx_on_when_idle_set(int8_t interface_id, bool rx_on)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur)) {
        return -1;
    }
    if (cur->ws_info->rx_off_when_idle == !rx_on) {
        return 0;
    }
    cur->ws_info->rx_off_when_idle = !rx_on;
    // Before the MAC starts this is only stored: ws_bootstrap_fhss_activate()
    // applies it. Afterwards the MAC applies it at once.
    if (cur->mac_api && (cur->lowpan_info & INTERFACE_NWK_ACTIVE)) {
        mac_helper_pib_boolean_set(cur, macRxOnWhenIdle, rx_on);
        if (rx_on) {
            cur->lowpan_info &= ~INTERFACE_NWK_CONF_MAC_RX_OFF_IDLE;
        } else {
            cur->lowpan_info |= INTERFACE_NWK_CONF_MAC_RX_OFF_IDLE;
        }
    }
    return 0;
}

void ws_management_lfn_settings_defaults(ws_lfn_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->listen_interval_ms = WS_LFN_LISTEN_INTERVAL_MS_DEFAULT;
    s->listen_interval_min_ms = WS_LFN_LISTEN_INTERVAL_MIN_MS_DEFAULT;
    s->listen_interval_max_ms = WS_LFN_LISTEN_INTERVAL_MAX_MS_DEFAULT;
    s->eapol_listen_interval_ms = WS_LFN_EAPOL_LISTEN_INTERVAL_MS_DEFAULT;
    s->rx_window_us = FHSS_LFN_RX_WINDOW_US_DEFAULT;
    s->rx_guard_us = FHSS_LFN_RX_GUARD_US_DEFAULT;
    s->close_retry_us = FHSS_LFN_CLOSE_RETRY_US_DEFAULT;
    s->uplink_guard_us = FHSS_LFN_UPLINK_GUARD_US_DEFAULT;
    s->lpas_interval_s = WS_LFN_LPAS_INTERVAL_S_DEFAULT;
    s->lnd_slot_time_ms = WS_LFN_LND_SLOT_TIME_MS_DEFAULT;
    s->lnd_slots = WS_LFN_LND_SLOTS_DEFAULT;
    s->lnd_response_delay_ms = WS_LFN_LND_RESPONSE_DELAY_MS_DEFAULT;
    s->lpcs_retry_intervals = WS_LFN_LPCS_RETRY_INTERVALS_DEFAULT;
    s->maintain_parent_time = WS_LFN_MAINTAIN_PARENT_TIME_DEFAULT;
    s->lgtk_max_mismatch_s = WS_LFN_LGTK_MAX_MISMATCH_S_DEFAULT;
    s->registration_lifetime_min = WS_LFN_REGISTRATION_LIFETIME_MIN_DEFAULT;
    s->drift_ppm = FHSS_LFN_DRIFT_PPM_DEFAULT;
    s->widening_max_us = FHSS_LFN_WIDENING_MAX_US_DEFAULT;
    s->bc_rx_window_us = FHSS_LFN_BC_RX_WINDOW_US_DEFAULT;
    s->bc_sync_only = false;
    s->bc_interval_ms = WS_LFN_BC_INTERVAL_MS_DEFAULT;
    s->bc_sync_period = WS_LFN_BC_SYNC_PERIOD_DEFAULT;
    s->lto = true;
    s->tx_offset_us = FHSS_LFN_TX_OFFSET_US_DEFAULT;
    s->tx_lead_us = FHSS_LFN_TX_LEAD_US_DEFAULT;
    s->tx_min_prep_us = FHSS_LFN_TX_MIN_PREP_US_DEFAULT;
    s->tx_ttl_intervals = FHSS_LFN_TX_TTL_INTERVALS_DEFAULT;
    s->lgtk_lifetime_s = WS_LFN_LGTK_LIFETIME_S_DEFAULT;
    s->lgtk_new_install_required = WS_LFN_LGTK_NEW_INSTALL_REQUIRED_DEFAULT;
    s->lgtk_new_activation_time = WS_LFN_LGTK_NEW_ACTIVATION_TIME_DEFAULT;
    s->lgtk_activation_guard_s = WS_LFN_LGTK_ACTIVATION_GUARD_S_DEFAULT;
}

int ws_management_lfn_settings_set(int8_t interface_id, const ws_lfn_settings_t *s)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur) || !s) {
        return -1;
    }
    if (!s->listen_interval_ms || s->listen_interval_ms > 0xffffff || s->listen_interval_min_ms > s->listen_interval_ms ||
            s->listen_interval_max_ms < s->listen_interval_ms || s->listen_interval_max_ms > 0xffffff ||
            !s->rx_window_us || s->rx_guard_us >= s->rx_window_us || !s->close_retry_us ||
            s->rx_guard_us >= s->bc_rx_window_us ||
            !s->lpas_interval_s || !s->lnd_slot_time_ms || !s->lnd_slots || !s->lpcs_retry_intervals ||
            !s->maintain_parent_time || !s->registration_lifetime_min || !s->bc_interval_ms || !s->bc_sync_period ||
            !s->tx_ttl_intervals || s->tx_min_prep_us > s->tx_lead_us || s->tx_lead_us > 60000 ||
            s->lgtk_lifetime_s < 60 || !s->lgtk_new_install_required || s->lgtk_new_install_required > 99 ||
            !s->lgtk_new_activation_time ||
            // the next LGTK must be installed before its activation point
            (uint32_t)s->lgtk_new_activation_time * (100 - s->lgtk_new_install_required) <= 100) {
        return -2;
    }
    cur->ws_info->lfn_cfg = *s;
    return 0;
}

int ws_management_lfn_parent_set(int8_t interface_id, bool enable)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur)) {
        return -1;
    }
#if defined(HAVE_WS_VERSION_1_1) && (defined(HAVE_WS_BORDER_ROUTER) || defined(HAVE_WS_ROUTER))
    // The border router or an FFN router (GS8 stage 5), not an LFN
    if (enable && (!ws_version_1_1(cur) || cur->bootstrap_mode == ARM_NWK_BOOTSTRAP_MODE_6LoWPAN_HOST)) {
        return -2;
    }
    cur->ws_info->lfn_parent = enable;
    return 0;
#else
    return enable ? -2 : 0;
#endif
}

int ws_management_lfn_lgtk_rotate(int8_t interface_id)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);

    if (!cur || !ws_info(cur)) {
        return -1;
    }
#if defined(HAVE_WS_VERSION_1_1) && defined(HAVE_WS_BORDER_ROUTER)
    if (!cur->ws_info->lfn_parent || ws_pae_controller_lgtk_rotate(cur) < 0) {
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}
