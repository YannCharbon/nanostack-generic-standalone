/*
 * Copyright (c) 2021, Pelion and affiliates.
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
#include "ns_rand.h"
#include "mbed-client-libservice/ns_trace.h"
#include <stdlib.h>
#include "mbed-client-libservice/common_functions.h"
#include "service_libs/trickle/trickle.h"
#include "service_libs/utils/ns_time.h"
#include "service_libs/etx/etx.h"
#include "service_libs/mac_neighbor_table/mac_neighbor_table.h"
#include "service_libs/nd_proxy/nd_proxy.h"
#include "service_libs/blacklist/blacklist.h"
#include "service_libs/random_early_detection/random_early_detection_api.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack/dhcp_service_api.h"
#include "nanostack/net_interface.h"
#include "nanostack/ws_management_api.h"
#include "nanostack/net_rpl.h"
#include "nanostack/mac/platform/topo_trace.h"
#include "nanostack/mac/mac_common_defines.h"
#include "nanostack/mac/sw_mac.h"
#include "nanostack/mac/mac_api.h"

#include "nwk_interface/protocol.h"
#include "ipv6_stack/protocol_ipv6.h"
#include "ipv6_stack/ipv6_routing_table.h"
#include "mpl/mpl.h"
#include "rpl/rpl_protocol.h"
#include "rpl/rpl_control.h"
#include "rpl/rpl_data.h"
#include "rpl/rpl_policy.h"
#include "common_protocols/icmpv6.h"
#include "common_protocols/icmpv6_radv.h"
#include "common_protocols/ipv6_constants.h"
#include "common_protocols/ip.h"
#include "libdhcpv6/libdhcpv6.h"
#include "libdhcpv6/libdhcpv6_vendordata.h"
#include "dhcpv6_client/dhcpv6_client_api.h"
#include "net_lib/net_dns_internal.h"
#include "6lowpan/lowpan_adaptation_interface.h"
#include "6lowpan/bootstraps/protocol_6lowpan.h"
#include "6lowpan/bootstraps/protocol_6lowpan_interface.h"
#include "6lowpan/mac/mac_helper.h"
#include "6lowpan/mac/mpx_api.h"
#include "6lowpan/mac/mac_ie_lib.h"

#include "6lowpan/ws/ws_bbr_api_internal.h"
#include "6lowpan/ws/ws_bootstrap.h"
#include "6lowpan/ws/ws_cfg_settings.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/ws/ws_common.h"
#include "6lowpan/ws/ws_config.h"
#include "6lowpan/ws/ws_eapol_auth_relay.h"
#include "6lowpan/ws/ws_eapol_pdu.h"
#include "6lowpan/ws/ws_eapol_relay.h"
#include "6lowpan/ws/ws_ie_lib.h"
#include "6lowpan/ws/ws_llc.h"
#include "6lowpan/ws/ws_neighbor_class.h"
#include "6lowpan/ws/ws_pae_controller.h"
#include "6lowpan/ws/ws_stats.h"
#include "6lowpan/ws/ws_bootstrap_lfn.h"
#include "6lowpan/ws/ws_lfn_parent.h"
#include "security/protocols/sec_prot_keys.h"
#include "nanostack/mac/fhss_ws_extension.h"
#include "common_protocols/icmpv6.h"
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
#include "nwk_interface/protocol_timer.h"
#endif

#ifdef HAVE_WS_HOST

#define TRACE_GROUP "wslf"

/* LPCS sent without an LPC before the LFN drops its keys and starts again
 * (a parent that cannot read them: a restarted network) [I] */
#define WS_LFN_LPCS_MAX 4

/*
 * Wi-SUN FAN 1.1 Limited Function Node (GS8 stage 3; docs/LFN_STACK.md).
 *
 * Join: LPAS sweeps on every channel (LUTT, NR, LUS, LND) until a parent
 * answers with an LPA in our next listen window; parent chosen (its UTT/BT,
 * US/BS, PANID, LTO, LBS); EAPOL with the parent as authenticator (shorter
 * listen interval meanwhile), skipped on a rejoin with valid keys; LPCS ->
 * LPC; DHCPv6 through the parent; address registration (NS with ARO) to the
 * parent, which registers the LFN on its behalf. No RPL, no MPL forwarding.
 *
 * Parent maintenance: the parent is lost after 1.5 x maintain_parent_time
 * sync periods with no sync frame from it (LTS, LPC; black-box results,
 * items 34 and 46); the LFN then rediscovers and rejoins without
 * re-authenticating.
 *
 * Sync (stage 4): every frame from the parent with an LBT-IE (LPA, LPC,
 * LTS) syncs the FHSS to its LFN broadcast schedule, whose slots the LFN
 * listens to; the FHSS keeps our unicast slots in the parent's time base.
 */

static void ws_lfn_listen_apply(protocol_interface_info_entry_t *cur, uint32_t interval_ms)
{
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    fhss_ws_lfn_listen_t l;

    ns_fhss_ws_lfn_listen_defaults(&l);
    l.listen_interval_ms = interval_ms;
    l.rx_window_us = c->rx_window_us;
    l.rx_guard_us = c->rx_guard_us;
    l.close_retry_us = c->close_retry_us;
    l.uplink_guard_us = c->uplink_guard_us;
    l.drift_ppm = c->drift_ppm;
    l.widening_max_us = c->widening_max_us;
    l.bc_rx_window_us = c->bc_rx_window_us;
    l.bc_sync_only = c->bc_sync_only;
    cur->ws_info->lfn.listen_interval_ms = interval_ms;
    int ret = ns_fhss_ws_lfn_listen_set(cur->ws_info->fhss_api, &l);
    if (ret) {
        tr_error("LFN listen schedule refused (%d)", ret);
    }
}

// Interval change alone (EAPOL), keeping the phase of the current slot
static void ws_lfn_interval_set(protocol_interface_info_entry_t *cur, uint32_t interval_ms)
{
    if (!interval_ms || interval_ms == cur->ws_info->lfn.listen_interval_ms) {
        return;
    }
    if (ns_fhss_ws_lfn_timing_set(cur->ws_info->fhss_api, 0, interval_ms, ns_sw_mac_read_current_timestamp(cur->mac_api)) == 0) {
        cur->ws_info->lfn.listen_interval_ms = interval_ms;
        tr_info("LFN listen interval %"PRIu32" ms", interval_ms);
    }
}

// The LFN's own unicast channel function (no broadcast schedule of its own),
// the network name, the PAN information and the GTK hash for the LLC
static void ws_lfn_network_configure(protocol_interface_info_entry_t *cur)
{
    fhss_ws_configuration_t fc = ws_common_get_current_fhss_configuration(cur);

    if (!cur->ws_info->lfn.keys_valid) {
        cur->ws_info->network_pan_id = 0xffff;
    }
    ws_common_regulatory_domain_config(cur, &cur->ws_info->hopping_schedule);
    ws_bootstrap_set_domain_rf_config(cur);
    ws_bootstrap_fhss_set_defaults(cur, &fc);
    ws_bootstrap_fhss_configure_channel_masks(cur, &fc);
    fc.ws_uc_channel_function = (fhss_ws_channel_functions)cur->ws_info->cfg->fhss.fhss_uc_channel_function;
    fc.ws_bc_channel_function = WS_FIXED_CHANNEL;
    fc.fhss_broadcast_interval = 0;
    fc.unicast_fixed_channel = ws_bootstrap_randomize_fixed_channel(cur->ws_info->cfg->fhss.fhss_uc_fixed_channel, cur->ws_info->hopping_schedule.number_of_channels, fc.channel_mask);
    fc.broadcast_fixed_channel = ws_bootstrap_randomize_fixed_channel(cur->ws_info->cfg->fhss.fhss_bc_fixed_channel, cur->ws_info->hopping_schedule.number_of_channels, fc.channel_mask);
    ns_fhss_ws_configuration_set(cur->ws_info->fhss_api, &fc);
    ws_bootstrap_llc_hopping_update(cur, &fc);
    ws_llc_set_network_name(cur, (uint8_t *)cur->ws_info->cfg->gen.network_name, strlen(cur->ws_info->cfg->gen.network_name));
    ws_llc_set_pan_information_pointer(cur, &cur->ws_info->pan_information);
    ws_llc_set_gtkhash(cur, ws_pae_controller_gtk_hash_ptr_get(cur));
}

static uint32_t ws_lfn_sync_period_s(protocol_interface_info_entry_t *cur)
{
    if (cur->ws_info->lfn.sync_period_s) {
        return cur->ws_info->lfn.sync_period_s;
    }
    return (cur->ws_info->lfn_cfg.bc_interval_ms * cur->ws_info->lfn_cfg.bc_sync_period + 999) / 1000;
}

/* An LPA heard in the discovery window: the frame, kept until the window
 * ends and the best parent is chosen (GS8 stage 5) */
typedef struct ws_lfn_lpa_candidate {
    mcps_data_ind_t ind;
    uint16_t routing_cost;
    uint16_t hdr_len, pl_len;
    uint8_t ie[];                   // header IEs, then payload IEs
} ws_lfn_lpa_candidate_t;

static void ws_lfn_candidates_clear(protocol_interface_info_entry_t *cur)
{
    for (int i = 0; i < 4; i++) {
        free(cur->ws_info->lfn.lpa[i]);
        cur->ws_info->lfn.lpa[i] = NULL;
    }
    cur->ws_info->lfn.disc_decide_s = 0;
}

static void ws_lfn_lpas_send(protocol_interface_info_entry_t *cur)
{
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    asynch_request_t req;

    /* The discovery window: lnd_response_delay_ms after the sweep starts,
       lnd_slots x lnd_slot_time_ms long; the parents answer in a random slot
       of it [S: LND-IE; M items 4-5] */
    uint32_t now = ns_sw_mac_read_current_timestamp(cur->mac_api);
    uint32_t window_ms = (uint32_t)c->lnd_slots * c->lnd_slot_time_ms;
    /* Never before the sweep can have ended: about 20 ms per channel [M:
       16.8 ms on Silicon Labs' LFN, item 1], plus 300 ms [I] */
    uint32_t delay_ms = c->lnd_response_delay_ms;
    uint32_t sweep_ms = (uint32_t)cur->ws_info->hopping_schedule.number_of_channels * 20 + 300;
    if (delay_ms < sweep_ms) {
        delay_ms = sweep_ms;
    }
    ws_lfn_candidates_clear(cur);
    /* A new first slot per sweep (LND-IE): discovery slot i is on our unicast
       channel of slot number disc_first_slot + i [M: Silicon Labs' LFN draws
       one per sweep and its parents send the LPA on that channel] */
    cur->ws_info->lfn.disc_first_slot = rand_get_16bit();
    if (window_ms && ns_fhss_ws_lfn_discovery_set(cur->ws_info->fhss_api, now + delay_ms * 1000u, window_ms * 1000u,
                                                  cur->ws_info->lfn.disc_first_slot, (uint32_t)c->lnd_slot_time_ms * 1000u) == 0) {
        cur->ws_info->lfn.disc_decide_s = cur->ws_info->uptime + (delay_ms + window_ms + 999) / 1000 + 1;
    }

    memset(&req, 0, sizeof(req));
    req.message_type = WS_FT_LPAS;
    req.wh_requested_ie_list.lutt_ie = true;
    req.wh_requested_ie_list.nr_ie = true;
    req.wh_requested_ie_list.lus_ie = true;
    req.wh_requested_ie_list.lnd_ie = true;
    // Our unicast schedule in an LCP-IE (tag of the LUS-IE) [S: wsbrd needs
    // it; M: Silicon Labs' LPAS]
    req.wp_requested_nested_ie_list.lcp_ie = true;
    req.wp_requested_nested_ie_list.net_name_ie = true;
    req.security.SecurityLevel = 0;
    // The channels of the plan, as an FFN's PAN advertisement solicits
    req.channel_list.channel_page = CHANNEL_PAGE_10;
    req.channel_list.next_channel_number = 0;
    ws_common_generate_channel_list(cur, req.channel_list.channel_mask, cur->ws_info->hopping_schedule.number_of_channels,
                                    cur->ws_info->hopping_schedule.regulatory_domain, cur->ws_info->hopping_schedule.operating_class,
                                    cur->ws_info->hopping_schedule.channel_plan_id);
    tr_info("LFN: LPAS sweep");
    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PAS, 1);
    ws_llc_asynch_request(cur, &req);
}

/* Before the first LPC tells the active key: the first LGTK installed (key
 * index 5-7) */
static void ws_lfn_tx_key_select(protocol_interface_info_entry_t *cur)
{
    int8_t i = ws_pae_controller_first_installed_key_index_get(cur);

    if (!mac_helper_default_key_index_get(cur) && i >= 0) {
        // As ws_bootstrap_nw_key_index_set(): the 4-way handshake may have
        // marked the key active already, so the PAE does not set it again
        mac_helper_security_auto_request_key_index_set(cur, i, i + WS_LGTK_KEY_INDEX_FIRST);
    }
}

static void ws_lfn_lpcs_send(protocol_interface_info_entry_t *cur)

{
    asynch_request_t req;

    memset(&req, 0, sizeof(req));
    req.message_type = WS_FT_LPCS;
    req.unicast = true;
    memcpy(req.dst_address, cur->ws_info->lfn.parent_eui64, 8);
    req.wh_requested_ie_list.lutt_ie = true;
    req.wh_requested_ie_list.lus_ie = true;
    // The network name, which wsbrd requires in an LPCS [S: ws_mngt.c; M:
    // Silicon Labs' LPCS]; our schedule is known from the LPAS (LUS tag 255)
    req.wp_requested_nested_ie_list.net_name_ie = true;
    req.security.SecurityLevel = mac_helper_default_security_level_get(cur);
    req.security.KeyIdMode = mac_helper_default_security_key_id_mode_get(cur);
    req.security.KeyIndex = mac_helper_default_key_index_get(cur);
    tr_info("LFN: LPCS to %s (key %u, level %u)", trace_array(req.dst_address, 8), req.security.KeyIndex, req.security.SecurityLevel);
    ws_llc_asynch_request(cur, &req);
    cur->ws_info->lfn.lpcs_count++;
    cur->ws_info->lfn.lpcs_timer_s = (cur->ws_info->lfn.listen_interval_ms * cur->ws_info->lfn_cfg.lpcs_retry_intervals + 999) / 1000 + 1;
}

/* The parent was heard (any secured frame, from the LLC): keep its
 * neighbour entry. Parent maintenance counts sync frames only. */
void ws_bootstrap_lfn_parent_heard(protocol_interface_info_entry_t *cur, const uint8_t *src64)
{
    if (!wisun_mode_host(cur) || !cur->ws_info->lfn.parent_known || memcmp(src64, cur->ws_info->lfn.parent_eui64, 8)) {
        return;
    }
    mac_neighbor_table_entry_t *n = mac_neighbor_table_address_discover(mac_neighbor_info(cur), src64, MAC_ADDR_MODE_64_BIT);
    if (n) {
        n->lifetime = n->link_lifetime;
    }
}

/* A frame from the parent with an LBT-IE: sync to its LFN broadcast
 * schedule (LBS-IE values from this frame or an earlier one), and keep the
 * parent (black-box item 34: maintenance counts sync frames) */
static void ws_lfn_sync(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_lbt_ie_t lbt;
    ws_lbs_ie_t lbs;
    llc_neighbour_req_t ni;

    if (ws_wh_lbs_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lbs) && lbs.broadcast_interval && lbs.broadcast_sync_period) {
        lfn->lbs_interval_ms = lbs.broadcast_interval;
        lfn->lbs_bsi = lbs.broadcast_secheduler_id;
        lfn->lbs_sync_period = lbs.broadcast_sync_period;
        lfn->sync_period_s = (lbs.broadcast_interval * lbs.broadcast_sync_period + 999) / 1000;
    }
    if (!ws_wh_lbt_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lbt) || !lfn->lbs_interval_ms ||
            !ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, false)) {
        return;
    }
    fhss_ws_lfn_bc_sync_t s = {
        .interval_ms = lfn->lbs_interval_ms,
        .bsi = lfn->lbs_bsi,
        .sync_period = lfn->lbs_sync_period,
        .channel_function = ni.ws_neighbor->fhss_data.bc_timing_info.broadcast_channel_function,
        .fixed_channel = ni.ws_neighbor->fhss_data.bc_timing_info.fixed_channel,
        .slot = lbt.slot_number,
        .offset_ms = lbt.interval_offset,
        .rx_timestamp_us = data->timestamp,
    };
    if (ns_fhss_ws_lfn_bc_sync(cur->ws_info->fhss_api, &s)) {
        tr_warn("LFN: sync refused (LBT slot %u, offset %"PRIu32" ms)", lbt.slot_number, (uint32_t)lbt.interval_offset);
        return;
    }
    lfn->parent_rx_s = cur->ws_info->uptime;
    tr_debug("LFN: parent heard (sync)");
}

/* During the discovery window: keep the LPA (at most 4: a better one
 * replaces the worst) */
static bool ws_lfn_lpa_keep(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_pan_information_t pan;
    int slot = -1;

    if (!lfn->disc_decide_s || lfn->parent_known || data->SrcAddrMode != MAC_ADDR_MODE_64_BIT) {
        return false;
    }
    memset(&pan, 0, sizeof(pan));
    if (!ws_wp_nested_pan_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &pan)) {
        pan.routing_cost = 0xffff;
    }
    for (int i = 0; i < 4; i++) {
        ws_lfn_lpa_candidate_t *c = lfn->lpa[i];
        if (c && !memcmp(c->ind.SrcAddr, data->SrcAddr, 8)) {
            slot = i;   // the same parent again: the newer one
            break;
        }
        if (!c) {
            if (slot < 0 || lfn->lpa[slot]) {
                slot = i;
            }
        } else if (slot < 0 || (lfn->lpa[slot] && (c->routing_cost > lfn->lpa[slot]->routing_cost ||
                                                   (c->routing_cost == lfn->lpa[slot]->routing_cost && c->ind.signal_dbm < lfn->lpa[slot]->ind.signal_dbm)))) {
            slot = i;   // the worst so far
        }
    }
    ws_lfn_lpa_candidate_t *worst = lfn->lpa[slot];
    if (worst && memcmp(worst->ind.SrcAddr, data->SrcAddr, 8) &&
            (worst->routing_cost < pan.routing_cost || (worst->routing_cost == pan.routing_cost && worst->ind.signal_dbm >= data->signal_dbm))) {
        return true;    // no better than the ones kept
    }
    ws_lfn_lpa_candidate_t *c = malloc(sizeof(*c) + ie_ext->headerIeListLength + ie_ext->payloadIeListLength);
    if (!c) {
        return true;
    }
    c->ind = *data;
    c->ind.msdu_ptr = NULL;
    c->ind.msduLength = 0;
    c->routing_cost = pan.routing_cost;
    c->hdr_len = ie_ext->headerIeListLength;
    c->pl_len = ie_ext->payloadIeListLength;
    memcpy(c->ie, ie_ext->headerIeList, c->hdr_len);
    memcpy(c->ie + c->hdr_len, ie_ext->payloadIeList, c->pl_len);
    free(worst);
    lfn->lpa[slot] = c;
    tr_info("LFN: LPA from %s kept (routing cost %u, %d dBm)", trace_array(data->SrcAddr, 8), pan.routing_cost, data->signal_dbm);
    return true;
}

static void ws_lfn_lpa_analyse(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext);

/* The discovery window has ended: the parent with the lowest routing cost
 * among those heard CAND_PARENT_THRESHOLD dB above the device's minimum
 * sensitivity (as an FFN chooses its candidate parents), the strongest on a
 * tie; without one, the same among those above the minimum [I] */
static void ws_lfn_lpa_choose(protocol_interface_info_entry_t *cur)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_lfn_lpa_candidate_t *best = NULL;
    int min_dbm = (int)cur->ws_info->device_min_sens - 174;
    bool best_good = false;

    for (int i = 0; i < 4; i++) {
        ws_lfn_lpa_candidate_t *c = lfn->lpa[i];
        if (!c || c->ind.signal_dbm < min_dbm) {
            if (c) {
                tr_info("LFN: LPA from %s too weak (%d dBm)", trace_array(c->ind.SrcAddr, 8), c->ind.signal_dbm);
            }
            continue;
        }
        bool good = c->ind.signal_dbm >= min_dbm + CAND_PARENT_THRESHOLD;
        if (!best || (good && !best_good) ||
                (good == best_good && (c->routing_cost < best->routing_cost ||
                                       (c->routing_cost == best->routing_cost && c->ind.signal_dbm > best->ind.signal_dbm)))) {
            best = c;
            best_good = good;
        }
    }
    lfn->disc_decide_s = 0;
    if (best) {
        mcps_data_ie_list_t ie = {
            .headerIeList = best->ie, .headerIeListLength = best->hdr_len,
            .payloadIeList = best->ie + best->hdr_len, .payloadIeListLength = best->pl_len,
        };
        tr_info("LFN: parent chosen: %s (routing cost %u, %d dBm)", trace_array(best->ind.SrcAddr, 8), best->routing_cost, best->ind.signal_dbm);
        ws_lfn_lpa_analyse(cur, &best->ind, &ie);
    }
    ws_lfn_candidates_clear(cur);
}

static void ws_lfn_lpa_analyse(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_utt_ie_t utt;
    ws_bt_ie_t bt;
    ws_us_ie_t us;
    ws_bs_ie_t bs;
    ws_panid_ie_t panid;
    ws_lto_ie_t lto;
    ws_flus_ie_t flus;
    llc_neighbour_req_t ni;

    if (lfn->parent_known || data->SrcAddrMode != MAC_ADDR_MODE_64_BIT) {
        return;
    }
    if (!ws_bootstrap_network_name_matches(ie_ext, cur->ws_info->cfg->gen.network_name)) {
        tr_debug("LFN: LPA from another network");
        return;
    }
    /* The parent's unicast schedule: FLUS-IE (dwell interval) and the LCP-IE
       of its tag [S: wisun-br-linux ws_mngt.c; M: Silicon Labs' LPA], or a
       US-IE (our parents before the black-box phase 2 work) */
    bool has_us = ws_wh_flus_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &flus) &&
                  ws_wp_nested_lcp_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, flus.channel_plan_tag, flus.dwell_interval, &us);
    if (!has_us) {
        has_us = ws_wp_nested_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &us);
    }
    if (!ws_wh_utt_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &utt) ||
            !ws_wh_bt_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &bt) ||
            !ws_wh_panid_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &panid) || !has_us ||
            !ws_wp_nested_bs_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &bs)) {
        tr_warn("LFN: LPA without UTT, BT, PANID, FLUS+LCP (or US) or BS: ignored");
        return;
    }
    if (!ws_bootstrap_validate_channel_plan(&us, cur) || !ws_bootstrap_validate_channel_function(&us, &bs)) {
        return;
    }
    // Keys belong to a PAN, as for an FFN (ws_pae_supp_nw_keys_valid_check()):
    // an LPA from another PAN (a restarted border router) needs EAPOL again
    if (lfn->keys_valid && cur->ws_info->network_pan_id != 0xffff && panid.panid != cur->ws_info->network_pan_id) {
        tr_info("LFN: LPA from PAN %04x, keys from PAN %04x: authentication again", panid.panid, cur->ws_info->network_pan_id);
        lfn->keys_valid = false;
        ws_bootstrap_event_discovery_start(cur);
        return;
    }
    // The parent's PAN and a neighbour entry for it
    cur->ws_info->network_pan_id = panid.panid;
    mac_helper_panid_set(cur, panid.panid);
    if (!ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, true)) {
        return;
    }
    ws_bootstrap_neighbor_set_stable(cur, data->SrcAddr);
    ni.neighbor->link_role = PRIORITY_PARENT_NEIGHBOUR;
    mac_neighbor_table_trusted_neighbor(mac_neighbor_info(cur), ni.neighbor, true);
    ws_neighbor_class_neighbor_unicast_time_info_update(ni.ws_neighbor, &utt, data->timestamp, (uint8_t *)data->SrcAddr);
    ws_neighbor_class_neighbor_unicast_schedule_set(cur, ni.ws_neighbor, &us, &cur->ws_info->hopping_schedule, data->SrcAddr);
    ws_neighbor_class_neighbor_broadcast_schedule_set(ni.ws_neighbor, &bs);
    ws_neighbor_class_neighbor_broadcast_time_info_update(ni.ws_neighbor, &bt, data->timestamp);
    // Its broadcast schedule, which our uplinks avoid (docs/LFN_MAC.md §3)
    ns_fhss_ws_set_parent(cur->ws_info->fhss_api, data->SrcAddr, &ni.ws_neighbor->fhss_data.bc_timing_info, true);
    memcpy(lfn->parent_eui64, data->SrcAddr, 8);
    lfn->parent_known = true;
    lfn->parent_rx_s = cur->ws_info->uptime;
    // The parent's LTO-IE: our offset and adjusted interval
    if (ws_wh_lto_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lto)) {
        uint32_t ivl = lto.adjusted_listening_interval;
        if (ivl && (ivl < cur->ws_info->lfn_cfg.listen_interval_min_ms || ivl > cur->ws_info->lfn_cfg.listen_interval_max_ms)) {
            ivl = 0;
        }
        if (ns_fhss_ws_lfn_timing_set(cur->ws_info->fhss_api, lto.offset, ivl, data->timestamp) == 0 && ivl) {
            lfn->listen_interval_ms = ivl;
        }
        tr_info("LFN: LTO offset %"PRIu32" ms, interval %"PRIu32" ms", (uint32_t)lto.offset, lfn->listen_interval_ms);
    }
    // Its LFN broadcast schedule (LBS, LBT)
    ws_lfn_sync(cur, data, ie_ext);
    cur->ws_info->lfn_cfg.listen_interval_ms = lfn->listen_interval_ms;   // the agreed interval from now on
    tr_info("LFN: parent %s, PAN %04x, sync period %"PRIu32" s", trace_array(data->SrcAddr, 8), panid.panid, ws_lfn_sync_period_s(cur));
    ws_stats_update(cur, STATS_WS_ASYNCH_RX_PA, 1);

    if (lfn->keys_valid) {
        // Rejoin: the keys are still valid, no EAPOL (black-box item 47)
        ws_bootstrap_event_configuration_start(cur);
        return;
    }
    ws_pae_controller_set_target(cur, panid.panid, lfn->parent_eui64);
    ws_bootstrap_event_authentication_start(cur);
}

static void ws_lfn_lpc_analyse(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_lfnver_ie_t ver;
    ws_lto_ie_t lto;

    if (!lfn->parent_known || memcmp(data->SrcAddr, lfn->parent_eui64, 8) || !data->Key.SecurityLevel) {
        return;
    }
    tr_debug("LFN: LPC from the parent (%s, key index %u)", data->DstAddrMode ? "unicast" : "broadcast", data->Key.KeyIndex);
    if (ws_wh_lto_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lto)) {
        uint32_t ivl = lto.adjusted_listening_interval;
        if (ns_fhss_ws_lfn_timing_set(cur->ws_info->fhss_api, lto.offset, ivl, data->timestamp) == 0 && ivl) {
            lfn->listen_interval_ms = ivl;
        }
    }
    ws_lfn_sync(cur, data, ie_ext);
    if (ws_wp_nested_lfn_version_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &ver)) {
        if (lfn->lpc_received && ver.lfn_version != lfn->lfn_version) {
            tr_info("LFN: LFN version %u -> %u", lfn->lfn_version, ver.lfn_version);
        }
        lfn->lfn_version = ver.lfn_version;
    }
    // The LGTK hash first: it makes a newly fetched LGTK fresh, so that it can
    // become the active one
    ws_lfn_parent_lgtk_hash_check(cur, ie_ext);
    // The parent's key index is the active LGTK (5-7), as an FFN learns its
    // GTK from a PAN configuration
    if (data->Key.KeyIndex >= WS_LGTK_KEY_INDEX_FIRST && data->Key.KeyIndex < WS_LGTK_KEY_INDEX_FIRST + LGTK_NUM &&
            data->Key.KeyIndex != mac_helper_default_key_index_get(cur)) {
        uint8_t index = data->Key.KeyIndex - WS_LGTK_KEY_INDEX_FIRST;
        tr_info("LFN: active LGTK index %u (key index %u)", index, data->Key.KeyIndex);
        ws_pae_controller_nw_key_index_update(cur, index);
        if (data->Key.KeyIndex != mac_helper_default_key_index_get(cur)) {
            mac_helper_security_auto_request_key_index_set(cur, index, data->Key.KeyIndex);
        }
    }
    if (!lfn->lpc_received) {
        lfn->lpc_received = true;
        lfn->lpcs_timer_s = 0;
        tr_info("LFN: LPC, LFN version %u", lfn->lfn_version);
        if (cur->nwk_bootstrap_state == ER_SCAN) {
            ws_bootstrap_event_operation_start(cur);
        }
    }
}

void ws_bootstrap_lfn_asynch_ind(struct protocol_interface_info_entry *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext, uint8_t message_type)
{
    if (cur->ws_info->weakest_received_rssi > data->signal_dbm) {
        cur->ws_info->weakest_received_rssi = data->signal_dbm;
    }
    if (data->SrcAddrMode != MAC_ADDR_MODE_64_BIT) {
        return;
    }
    ws_stats_update(cur, STATS_WS_ASYNCH_RX, 1);
    switch (message_type) {
        case WS_FT_LPA:
            // In the discovery window: kept and compared at its end;
            // otherwise (a parent that answers in a listen window) at once
            if (!ws_lfn_lpa_keep(cur, data, ie_ext)) {
                ws_lfn_lpa_analyse(cur, data, ie_ext);
            }
            break;
        case WS_FT_LPC:
            ws_lfn_lpc_analyse(cur, data, ie_ext);
            break;
        case WS_FT_LTS:
            if (cur->ws_info->lfn.parent_known && !memcmp(data->SrcAddr, cur->ws_info->lfn.parent_eui64, 8) && data->Key.SecurityLevel) {
                ws_lfn_sync(cur, data, ie_ext);
            }
            break;
        default:
            // An LFN hears only what is sent to it in its windows
            break;
    }
}

void ws_bootstrap_lfn_asynch_confirm(struct protocol_interface_info_entry *interface, uint8_t asynch_message)
{
    (void)asynch_message;
    ws_stats_update(interface, STATS_WS_ASYNCH_TX, 1);
}

/* Address registration: NS with ARO to the parent, which registers the
 * address on our behalf (6LoWPAN-ND, RFC 8505) */
void ws_bootstrap_lfn_address_register(protocol_interface_info_entry_t *cur)
{
    uint8_t ll_parent[16];
    aro_t aro;
    bool sent = false;

    if (!wisun_mode_host(cur) || !cur->ws_info->lfn.parent_known) {
        return;
    }
    ws_common_create_ll_address(ll_parent, cur->ws_info->lfn.parent_eui64);
    ns_list_foreach(if_address_entry_t, addr, &cur->ip_addresses) {
        if (addr_is_ipv6_link_local(addr->address)) {
            continue;
        }
        memset(&aro, 0, sizeof(aro));
        aro.status = ARO_SUCCESS;
        aro.present = true;
        memcpy(aro.eui64, cur->mac, 8);
        aro.lifetime = cur->ws_info->lfn_cfg.registration_lifetime_min;
        buffer_t *buf = icmpv6_build_ns(cur, ll_parent, addr->address, true, false, &aro);
        if (buf) {
            tr_info("LFN: ARO %s to %s, %u min", trace_ipv6(addr->address), trace_ipv6(ll_parent), aro.lifetime);
            protocol_push(buf);
            sent = true;
        }
    }
    if (sent) {
        // Re-registration at 92 % of the lifetime (black-box item 52)
        cur->ws_info->aro_registration_timer = 0;
        cur->ws_info->lfn.addr_registered = true;
        uint32_t t = (uint32_t)cur->ws_info->lfn_cfg.registration_lifetime_min * 60 * 92 / 100;
        cur->ws_info->lfn.lpas_timer_s = 0;
        cur->ws_info->lfn.reregister_s = t ? t : 1;
        if (cur->nwk_bootstrap_state != ER_BOOTSTRAP_DONE) {
            ws_bootstrap_event_routing_ready(cur);
        }
    }
}

void ws_bootstrap_lfn_event_handler(protocol_interface_info_entry_t *cur, arm_event_s *event)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_bootstrap_event_type_e event_type = (ws_bootstrap_event_type_e)event->event_type;

    switch (event_type) {
        case WS_INIT_EVENT:
            tr_debug("tasklet init");
            break;
        case WS_DISCOVERY_START:
            tr_info("LFN discovery start");
            protocol_mac_reset(cur);
            ws_llc_reset(cur);
            lowpan_adaptation_interface_reset(cur->id);
            ipv6_destination_cache_clean(cur->id);
            blacklist_clear();
            ws_bootstrap_asynch_trickle_stop(cur);
            ws_bootstrap_packet_congestion_init(cur);
            if (!lfn->keys_valid) {
                cur->ws_info->pending_key_index_info.state = NO_PENDING_PROCESS;
                cur->mac_parameters.mac_default_key_index = 0;
                ws_pae_controller_supp_init(cur);
            }
            ws_bootstrap_neighbor_list_clean(cur);
            lfn->parent_known = false;
            lfn->lpc_received = false;
            lfn->addr_registered = false;
            lfn->eapol_interval = false;
            lfn->sync_period_s = 0;
            lfn->lbs_interval_ms = 0;
            lfn->reregister_s = 0;
            lfn->lgtk_mismatch_timer_s = 0;
            lfn->lgtk_request_sent = false;
            ws_lfn_network_configure(cur);
            // The receiver is off outside the listen windows
            cur->ws_info->rx_off_when_idle = true;

            ws_lfn_listen_apply(cur, cur->ws_info->lfn_cfg.listen_interval_ms);
            ws_bootstrap_fhss_activate(cur);
            // No PAN yet, so MLME-START did not synchronize the FHSS: the
            // listen schedule starts now
            ns_fhss_ws_lfn_listen_start(cur->ws_info->fhss_api);
            ws_bootstrap_configure_max_retries(cur, WS_MAX_FRAME_RETRIES_BOOTSTRAP);
            ws_bootstrap_configure_data_request_restart(cur, WS_CCA_REQUEST_RESTART_MAX, WS_TX_REQUEST_RESTART_MAX_BOOTSTRAP, WS_REQUEST_RESTART_BLACKLIST_MIN, WS_REQUEST_RESTART_BLACKLIST_MAX);
            ws_bootstrap_configure_csma_ca_backoffs(cur, WS_MAX_CSMA_BACKOFFS, WS_MAC_MIN_BE, WS_MAC_MAX_BE);
            cur->nwk_bootstrap_state = ER_ACTIVE_SCAN;
            lfn->lpas_timer_s = 1;
            break;
        case WS_CONFIGURATION_START:
            // Authenticated (or rejoining with valid keys): back to the
            // agreed interval, then LPCS -> LPC
            tr_info("LFN configuration start");
            lfn->keys_valid = true;
            if (lfn->eapol_interval) {
                lfn->eapol_interval = false;
                ws_lfn_interval_set(cur, cur->ws_info->lfn_cfg.listen_interval_ms);
            }
            cur->nwk_bootstrap_state = ER_SCAN;
            lfn->lpc_received = false;
            lfn->lpcs_count = 0;
            ws_lfn_tx_key_select(cur);
            ws_lfn_lpcs_send(cur);
            break;
        case WS_OPERATION_START:
            tr_info("LFN operation start");
            ws_bootstrap_ip_stack_activate(cur);
            ws_bootstrap_configure_max_retries(cur, WS_MAX_FRAME_RETRIES);
            ws_bootstrap_configure_data_request_restart(cur, WS_CCA_REQUEST_RESTART_MAX, WS_TX_REQUEST_RESTART_MAX, WS_REQUEST_RESTART_BLACKLIST_MIN, WS_REQUEST_RESTART_BLACKLIST_MAX);
            cur->nwk_bootstrap_state = ER_RPL_SCAN;
            {
                uint8_t ll_parent[16];
                ws_common_create_ll_address(ll_parent, lfn->parent_eui64);
                // No RPL: everything off-link goes through the parent
                ipv6_route_table_remove_info(cur->id, ROUTE_STATIC, NULL);
                ipv6_route_add(NULL, 0, cur->id, ll_parent, ROUTE_STATIC, 0xffffffff, 0);

                if (ns_list_count(&cur->ip_addresses) > 1) {
                    // Rejoin with an address kept: register it again
                    ws_bootstrap_lfn_address_register(cur);
                } else {
                    // DHCPv6 through the parent; no prefix hint (no RPL)
                    ws_dhcp_client_address_request(cur, NULL, ll_parent);
                }
            }
            break;
        case WS_ROUTING_READY:
            tr_info("LFN operational");
            ws_pae_controller_bootstrap_done(cur);
            cur->ws_info->connected_time = cur->ws_info->uptime;
            ws_bootstrap_state_change(cur, ER_BOOTSTRAP_DONE);
            break;
        case WS_FAST_DISCONNECT:
        case WS_NORMAL_DISCONNECT:
            ws_bootstrap_event_discovery_start(cur);
            break;
        default:
            tr_err("Invalid event received");
            break;
    }
}

void ws_bootstrap_lfn_state_machine(protocol_interface_info_entry_t *cur)
{
    switch (cur->nwk_bootstrap_state) {
        case ER_WAIT_RESTART:
            ws_bootstrap_event_discovery_start(cur);
            break;
        case ER_PANA_AUTH:
            tr_info("LFN authentication start");
            // A shorter listen interval while EAPOL runs (black-box item 10)
            if (cur->ws_info->lfn_cfg.eapol_listen_interval_ms) {
                cur->ws_info->lfn.eapol_interval = true;
                ws_lfn_interval_set(cur, cur->ws_info->lfn_cfg.eapol_listen_interval_ms);
            }
            ws_bootstrap_start_authentication(cur);
            break;
        case ER_BOOTSTRAP_DONE:
            nwk_bootstrap_state_update(ARM_NWK_BOOTSTRAP_READY, cur);
            break;
        default:
            break;
    }
}

#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
uint32_t ws_bootstrap_lfn_seconds_timer_next(protocol_interface_info_entry_t *cur)
{
    ws_lfn_state_t *lfn;
    uint32_t next = UINT32_MAX;

    if (!wisun_mode_host(cur) || !(cur->lowpan_info & INTERFACE_NWK_ACTIVE)) {
        return next;
    }
    lfn = &cur->ws_info->lfn;
    if (cur->nwk_bootstrap_state == ER_ACTIVE_SCAN && !lfn->parent_known && lfn->lpas_timer_s) {
        next = protocol_timer_next_min(next, lfn->lpas_timer_s);
    }
    if (lfn->disc_decide_s) {
        uint32_t up = cur->ws_info->uptime;
        next = protocol_timer_next_min(next, lfn->disc_decide_s > up ? lfn->disc_decide_s - up : 1);
    }
    if (cur->nwk_bootstrap_state == ER_SCAN && !lfn->lpc_received && lfn->lpcs_timer_s) {
        next = protocol_timer_next_min(next, lfn->lpcs_timer_s);
    }
    if (lfn->reregister_s) {
        next = protocol_timer_next_min(next, lfn->reregister_s);
    }
    if (lfn->lgtk_mismatch_timer_s) {
        next = protocol_timer_next_min(next, lfn->lgtk_mismatch_timer_s);
    }
    if (lfn->parent_known && cur->nwk_bootstrap_state != ER_PANA_AUTH) {
        // Lost on the first call that sees more than the limit since the
        // last frame (the uptime is incremented before this timer runs)
        uint32_t limit = ws_lfn_sync_period_s(cur) * cur->ws_info->lfn_cfg.maintain_parent_time * 3 / 2;
        uint32_t since = cur->ws_info->uptime - lfn->parent_rx_s;
        next = protocol_timer_next_min(next, since < limit ? limit - since + 1 : 1);
    }
    return next;
}

#endif
void ws_bootstrap_lfn_seconds_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    ws_lfn_state_t *lfn;

    if (!wisun_mode_host(cur) || !(cur->lowpan_info & INTERFACE_NWK_ACTIVE)) {
        return;
    }
    lfn = &cur->ws_info->lfn;
    // The end of a discovery window: choose among the LPAs heard
    if (lfn->disc_decide_s && cur->ws_info->uptime >= lfn->disc_decide_s) {
        ws_lfn_lpa_choose(cur);
    }
    // Discovery: an LPAS sweep every lpas_interval_s until an LPA comes
    if (cur->nwk_bootstrap_state == ER_ACTIVE_SCAN && !lfn->parent_known && lfn->lpas_timer_s) {
        if (lfn->lpas_timer_s > seconds) {
            lfn->lpas_timer_s -= seconds;
        } else {
            ws_lfn_lpas_send(cur);
            lfn->lpas_timer_s = cur->ws_info->lfn_cfg.lpas_interval_s;
        }
    }
    // Configuration: LPCS again if no LPC came
    if (cur->nwk_bootstrap_state == ER_SCAN && !lfn->lpc_received && lfn->lpcs_timer_s) {
        if (lfn->lpcs_timer_s > seconds) {
            lfn->lpcs_timer_s -= seconds;
        } else if (lfn->lpcs_count >= WS_LFN_LPCS_MAX) {
            // The parent does not answer: our keys may be stale (a restarted
            // network with the same PAN ID). Discovery again, with EAPOL [I]
            tr_warn("LFN: no LPC after %u LPCS: discovery and authentication again", lfn->lpcs_count);
            lfn->lpcs_timer_s = 0;
            lfn->keys_valid = false;
            ws_bootstrap_event_discovery_start(cur);
        } else {
            ws_lfn_lpcs_send(cur);
        }
    }
    // Address re-registration
    if (lfn->reregister_s) {
        if (lfn->reregister_s > seconds) {
            lfn->reregister_s -= seconds;
        } else {
            lfn->reregister_s = 0;
            ws_bootstrap_lfn_address_register(cur);
        }
    }
    ws_lfn_parent_lgtk_timer(cur, seconds);
    // Parent maintenance: lost after 1.5 x maintain_parent_time sync periods
    // without a frame from it (black-box item 46)
    if (lfn->parent_known && cur->nwk_bootstrap_state != ER_PANA_AUTH) {
        uint32_t limit = ws_lfn_sync_period_s(cur) * cur->ws_info->lfn_cfg.maintain_parent_time * 3 / 2;
        if (cur->ws_info->uptime - lfn->parent_rx_s > limit) {
            tr_warn("LFN: parent %s lost (no sync frame for %"PRIu32" s)", trace_array(lfn->parent_eui64, 8), limit);
            ns_fhss_ws_lfn_bc_stop(cur->ws_info->fhss_api);
            ws_stats_update(cur, STATS_WS_STATE_1, 0);
            ws_bootstrap_event_discovery_start(cur);
            lfn->parent_known = false;
        }
    }
}

#endif
