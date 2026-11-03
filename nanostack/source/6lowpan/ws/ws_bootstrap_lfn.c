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
#include "common_protocols/ipv6.h"
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
    l.bc_widening_max_us = c->bc_widening_max_us;
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

/* The parent search starts this long after the last sync frame: the sync
 * periods of the setting, plus one LFN broadcast interval, since a parent
 * may send a sync frame one slot late (its radio busy at the slot) and the
 * search must not start while the LFN's window for the next one is open
 * (Eco profile runs: the search's LPAS sweep took the radio 0.25 s before
 * the sync frame came) */
static uint32_t ws_lfn_search_limit_s(protocol_interface_info_entry_t *cur)
{
    uint32_t bc_ms = cur->ws_info->lfn.lbs_interval_ms ? cur->ws_info->lfn.lbs_interval_ms : cur->ws_info->lfn_cfg.bc_interval_ms;

    return ws_lfn_sync_period_s(cur) * cur->ws_info->lfn_cfg.search_sync_periods + (bc_ms + 999) / 1000;
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
    uint32_t window_ms = (uint32_t)(cur->ws_info->lfn.disc_slots ? cur->ws_info->lfn.disc_slots : c->lnd_slots) * c->lnd_slot_time_ms;
    /* Never before the sweep can have ended: about 20 ms per channel [M:
       16.8 ms on Silicon Labs' LFN, item 1], plus 300 ms [I] */
    /* A background check sends parent_check_sweeps sweeps back to back: an
       FFN hears an LPAS only on the channel it listens on, and it may be on
       its broadcast channel or hop away during one sweep [I] */
    uint8_t sweeps = 1;
    if (cur->ws_info->lfn.check && c->parent_check_sweeps > 1) {
        sweeps = c->parent_check_sweeps;
    }
    uint32_t delay_ms = c->lnd_response_delay_ms;
    uint32_t sweep_ms = (uint32_t)sweeps * cur->ws_info->hopping_schedule.number_of_channels * 20 + 300;
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
    for (uint8_t i = 0; i < sweeps; i++) {
        tr_info("LFN: LPAS sweep");
        ws_stats_update(cur, STATS_WS_ASYNCH_TX_PAS, 1);
        ws_llc_asynch_request(cur, &req);
    }
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

/* An acknowledged uplink: consecutive failures to the parent start a
 * parent search (ws_bootstrap_lfn_seconds_timer) */
void ws_bootstrap_lfn_uplink_result(protocol_interface_info_entry_t *cur, const uint8_t *dst64, bool success)
{
    ws_lfn_state_t *lfn;

    if (!wisun_mode_host(cur) || !cur->ws_info->lfn.parent_known || memcmp(dst64, cur->ws_info->lfn.parent_eui64, 8)) {
        return;
    }
    lfn = &cur->ws_info->lfn;
    if (success) {
        lfn->uplink_fails = 0;
    } else if (lfn->uplink_fails < 255) {
        lfn->uplink_fails++;
    }
}

/* ---- Better parent while attached (docs/LFN_STACK.md §9) */

/* The parent's link is weak: its smoothed RSL below the candidate-parent
 * threshold, DEVICE_MIN_SENS + CAND_PARENT_THRESHOLD [S: Wi-SUN FAN
 * candidate parent set, ws_common_defines.h] */
static bool ws_lfn_parent_weak(protocol_interface_info_entry_t *cur)
{
    uint16_t x8 = cur->ws_info->lfn.parent_rsl_x8;
    return x8 && (x8 >> WS_RSL_SCALING) < cur->ws_info->device_min_sens + CAND_PARENT_THRESHOLD;
}

static uint32_t ws_lfn_check_interval(protocol_interface_info_entry_t *cur)
{
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    if (ws_lfn_parent_weak(cur) && c->parent_check_weak_s) {
        return c->parent_check_weak_s;
    }
    return c->parent_check_s;
}

/* The next background check: uniformly 0.5-1.5 x the interval from now
 * (LFNs that joined together do not sweep together) [I] */
static void ws_lfn_check_schedule(protocol_interface_info_entry_t *cur)
{
    uint32_t ivl = ws_lfn_check_interval(cur);
    if (!ivl) {
        cur->ws_info->lfn.check_next_s = 0;
        return;
    }
    cur->ws_info->lfn.check_next_s = cur->ws_info->uptime + ivl / 2 + rand_get_32bit() % (ivl + 1);
}

/* The link became weak: the next check no later than the weak interval */
static void ws_lfn_check_weak_pull(protocol_interface_info_entry_t *cur)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    uint32_t w = cur->ws_info->lfn_cfg.parent_check_weak_s;
    if (w && lfn->check_next_s && ws_lfn_parent_weak(cur) && lfn->check_next_s - cur->ws_info->uptime > w * 3 / 2) {
        ws_lfn_check_schedule(cur);
        tr_info("LFN: parent link weak (RSL %d dBm): next check in %"PRIu32" s",
                (int)((lfn->parent_rsl_x8 + 4) >> WS_RSL_SCALING) - 174, lfn->check_next_s - cur->ws_info->uptime);
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
    // The parent's RSL, smoothed as Wi-SUN's RSL EWMA (1/8) [S: Wi-SUN FAN
    // RSL EWMA; Nanostack ws_neighbor_class_rsl_in_calculate()]
    uint16_t rsl = ws_neighbor_class_rsl_from_dbm_calculate(data->signal_dbm);
    lfn->parent_rsl_x8 = lfn->parent_rsl_x8 ? lfn->parent_rsl_x8 + rsl - (lfn->parent_rsl_x8 >> WS_RSL_SCALING) : rsl << WS_RSL_SCALING;
    ws_lfn_check_weak_pull(cur);
    tr_debug("LFN: parent heard (sync)");
}

/* During the discovery window: keep the LPA (at most 4: a better one
 * replaces the worst) */
static bool ws_lfn_lpa_keep(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_pan_information_t pan;
    int slot = -1;

    if (!lfn->disc_decide_s || (lfn->parent_known && !lfn->searching) || data->SrcAddrMode != MAC_ADDR_MODE_64_BIT) {
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

/* A background check: the parent's own LPA. Its LTO-IE is applied as the
 * parent applies it when we acknowledge the LPA, and its UTT and BT refresh
 * its timing in our neighbour table. */
static void ws_lfn_lpa_refresh(protocol_interface_info_entry_t *cur, ws_lfn_lpa_candidate_t *c)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    mcps_data_ie_list_t ie = {
        .headerIeList = c->ie, .headerIeListLength = c->hdr_len,
        .payloadIeList = c->ie + c->hdr_len, .payloadIeListLength = c->pl_len,
    };
    ws_lto_ie_t lto;
    ws_utt_ie_t utt;
    ws_bt_ie_t bt;
    llc_neighbour_req_t ni;

    if (ws_bootstrap_neighbor_info_request(cur, c->ind.SrcAddr, &ni, false)) {
        if (ws_wh_utt_read(ie.headerIeList, ie.headerIeListLength, &utt)) {
            ws_neighbor_class_neighbor_unicast_time_info_update(ni.ws_neighbor, &utt, c->ind.timestamp, c->ind.SrcAddr);
        }
        if (ws_wh_bt_read(ie.headerIeList, ie.headerIeListLength, &bt)) {
            ws_neighbor_class_neighbor_broadcast_time_info_update(ni.ws_neighbor, &bt, c->ind.timestamp);
        }
    }
    if (ws_wh_lto_read(ie.headerIeList, ie.headerIeListLength, &lto)) {
        uint32_t ivl = lto.adjusted_listening_interval;
        if (ivl && (ivl < cur->ws_info->lfn_cfg.listen_interval_min_ms || ivl > cur->ws_info->lfn_cfg.listen_interval_max_ms)) {
            ivl = 0;
        }
        if (ns_fhss_ws_lfn_timing_set(cur->ws_info->fhss_api, lto.offset, ivl, c->ind.timestamp) == 0 && ivl) {
            lfn->listen_interval_ms = ivl;
        }
    }
    ws_lfn_sync(cur, &c->ind, &ie);
    if (c->routing_cost != 0xffff) {
        lfn->parent_cost = c->routing_cost;
    }
}

/* A background check: is the candidate better than the parent? The RSL
 * classes of the candidate-parent rule first, with Wi-SUN's hysteresis: a
 * parent whose smoothed RSL is under DEVICE_MIN_SENS + CAND_PARENT_THRESHOLD
 * (weak) gives way to a candidate CAND_PARENT_HYSTERISIS dB above it (good)
 * [S: Wi-SUN FAN CAND_PARENT_THRESHOLD and CAND_PARENT_HYSTERESIS,
 * ws_bootstrap.c candidate parents]; two weak links: the candidate must be
 * 2 x CAND_PARENT_HYSTERISIS dB stronger [I]; two good links: its routing
 * cost must be lower by parent_switch_cost (192 = RFC 6719
 * PARENT_SWITCH_THRESHOLD, MRHOF's switch hysteresis) [S]. A good parent
 * never gives way to a weak candidate. */
static bool ws_lfn_better(protocol_interface_info_entry_t *cur, const ws_lfn_lpa_candidate_t *c, const ws_lfn_lpa_candidate_t *own)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    int thr = cur->ws_info->device_min_sens + CAND_PARENT_THRESHOLD;
    int now_rsl = lfn->parent_rsl_x8 ? (lfn->parent_rsl_x8 + 4) >> WS_RSL_SCALING : (own ? own->ind.signal_dbm + 174 : 0);
    int new_rsl = c->ind.signal_dbm + 174;
    uint32_t now_cost = own ? own->routing_cost : lfn->parent_cost;
    bool better;

    if (now_rsl < thr) {
        better = new_rsl >= thr + CAND_PARENT_HYSTERISIS || new_rsl >= now_rsl + 2 * CAND_PARENT_HYSTERISIS;
    } else if (new_rsl < thr) {
        better = false;
    } else {
        better = (uint32_t)c->routing_cost + cur->ws_info->lfn_cfg.parent_switch_cost <= now_cost;
    }
    tr_info("LFN: check: parent %d dBm cost %"PRIu32", %s %d dBm cost %u: %s", now_rsl - 174, now_cost,
            trace_array(c->ind.SrcAddr, 8), new_rsl - 174, c->routing_cost, better ? "better" : "not better");
    return better;
}

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
    lfn->disc_slots = 0;
    if (lfn->searching && lfn->check) {
        // A background check: the parent is kept unless a better one answered
        lfn->searching = false;
        lfn->check = false;
        ws_lfn_lpa_candidate_t *own = NULL;
        for (int i = 0; i < 4; i++) {
            if (lfn->lpa[i] && !memcmp(lfn->lpa[i]->ind.SrcAddr, lfn->parent_eui64, 8)) {
                own = lfn->lpa[i];
            }
        }
        if (own) {
            // Its LPA's LTO-IE moved our slots in its view: follow it, and
            // refresh its timing (the release below needs its channel)
            ws_lfn_lpa_refresh(cur, own);
        }
        if (!best || best == own || !ws_lfn_better(cur, best, own)) {
            tr_info("LFN: better-parent check: parent kept");
            ws_lfn_candidates_clear(cur);
            return;
        }
        tr_info("LFN: better parent: %s -> %s (routing cost %u, %d dBm)", trace_array(lfn->parent_eui64, 8),
                trace_array(best->ind.SrcAddr, 8), best->routing_cost, best->ind.signal_dbm);
        lfn->searching = true;  // the switch below
    }
    if (lfn->searching) {
        // A search while keeping the parent: attach to the best parent that
        // answered, the same one included (a restarted parent has a new LFN
        // broadcast schedule and applies the LTO-IE of its LPA), without
        // EAPOL; nobody: keep the parent
        lfn->searching = false;
        if (!best) {
            tr_info("LFN: parent search: no parent heard, parent kept");
            ws_lfn_candidates_clear(cur);
            return;
        }
        if (!memcmp(best->ind.SrcAddr, lfn->parent_eui64, 8)) {
            tr_info("LFN: parent search: the parent answered, attaching again");
        } else {
            tr_info("LFN: parent changed: %s -> %s (routing cost %u, %d dBm)", trace_array(lfn->parent_eui64, 8),
                    trace_array(best->ind.SrcAddr, 8), best->routing_cost, best->ind.signal_dbm);
        }
        if (memcmp(best->ind.SrcAddr, lfn->parent_eui64, 8)) {
            // The old parent is released after our registration with the new one
            memcpy(lfn->old_parent_eui64, lfn->parent_eui64, 8);
            lfn->release_at_s = UINT32_MAX;
        }
        ns_fhss_ws_lfn_bc_stop(cur->ws_info->fhss_api);
        lfn->parent_known = false;
        lfn->lpc_received = false;
        lfn->uplink_fails = 0;
    }
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
    lfn->parent_rsl_x8 = 0;
    {
        ws_pan_information_t pan;
        memset(&pan, 0, sizeof(pan));
        lfn->parent_cost = ws_wp_nested_pan_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &pan) ? pan.routing_cost : 0xffff;
    }
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
    if (lfn->lpc_received && cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE) {
        // New LGTKs from a group-key handshake: stored once they work
        ws_pae_controller_nw_key_valid(cur, NULL);
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

/* RFC 9685 multicast subscriptions: the interface's groups of realm-local
 * scope or wider (those the application joins with SOCKET_IPV6_JOIN_GROUP,
 * and ff03::1, which Silicon Labs' LFN also registers [M: black-box item
 * 19]), except the MPL domain's ff03::fc and the all-routers groups (an LFN
 * is no router) */
static bool ws_lfn_group_registrable(const uint8_t *group)
{
    return addr_ipv6_multicast_scope(group) >= IPV6_SCOPE_REALM_LOCAL && !addr_ipv6_equal(group, ADDR_ALL_MPL_FORWARDERS) &&
           !addr_ipv6_equal(group, ADDR_REALM_LOCAL_ALL_ROUTERS) && !addr_ipv6_equal(group, ADDR_SITE_LOCAL_ALL_ROUTERS);
}

/* A release NS reached the old parent (MAC ACK) */
static void ws_lfn_release_ack_cb(buffer_t *buf, uint8_t status)
{
    if (status == SOCKET_TX_DONE && buf->interface && ws_info(buf->interface)) {
        buf->interface->ws_info->lfn.release_acked++;
    }
}

static bool ws_lfn_mc_send(protocol_interface_info_entry_t *cur, const uint8_t ll_dst[16], const uint8_t *group, uint16_t lifetime, bool release)
{
    // A new TID for each registration [I: RFC 8505 §5.2 lets the 6LN
    // increment it; our parents ignore it]
    buffer_t *buf = icmpv6_build_ns_earo_mc(cur, ll_dst, group, lifetime, ++cur->ws_info->lfn.earo_tid);
    if (buf) {
        tr_info("LFN: EARO multicast %s to %s, %u min", trace_ipv6(group), trace_ipv6(ll_dst), lifetime);
        if (release) {
            buf->ack_receive_cb = ws_lfn_release_ack_cb;
        }
        protocol_push(buf);
        return true;
    }
    return false;
}

void ws_bootstrap_lfn_group_change(protocol_interface_info_entry_t *cur, const uint8_t group[16], bool joined)
{
    if (!cur || !ws_info(cur) || !wisun_mode_host(cur) || !cur->ws_info->lfn.parent_known ||
            !cur->ws_info->lfn.addr_registered || !ws_lfn_group_registrable(group)) {
        return;     // registered with the addresses once the LFN is operational
    }
    uint8_t ll_parent[16];
    ws_common_create_ll_address(ll_parent, cur->ws_info->lfn.parent_eui64);
    ws_lfn_mc_send(cur, ll_parent, group, joined ? cur->ws_info->lfn_cfg.registration_lifetime_min : 0, false);
}

/* The parent left for a better one: its registrations end (lifetime 0, RFC
 * 6775/8505), so that it drops its route and its DAO target for our
 * addresses, its copies of our groups, and us as a child. Sent once the new
 * parent's route has reached the border router and the old parent has had
 * two of our listen intervals to deliver what it still held for us
 * (parent_release_s + 2 listen intervals after our registration with the new
 * parent). A lost release would leave the old parent publishing our address
 * (its DAO refreshes would take the route back): the NS are sent again every
 * WS_LFN_RELEASE_RETRY_S until each was acknowledged at the MAC (the NA of a
 * successful ARO is omitted in Wi-SUN), WS_LFN_RELEASE_TRIES tries in all [I] */
#define WS_LFN_RELEASE_TRIES 4
#define WS_LFN_RELEASE_RETRY_S 10
static void ws_lfn_old_parent_release(protocol_interface_info_entry_t *cur)
{
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    uint8_t ll[16];
    aro_t aro;

    lfn->release_at_s = 0;
    if (lfn->release_tries && lfn->release_acked >= lfn->release_sent) {
        tr_info("LFN: old parent %s released (acknowledged, %u tries)", trace_array(lfn->old_parent_eui64, 8), lfn->release_tries);
        return;
    }
    if (lfn->release_tries >= WS_LFN_RELEASE_TRIES) {
        tr_warn("LFN: old parent %s: release not acknowledged after %u tries", trace_array(lfn->old_parent_eui64, 8), lfn->release_tries);
        return;
    }
    if (!memcmp(lfn->old_parent_eui64, lfn->parent_eui64, 8) ||
            !mac_neighbor_table_address_discover(mac_neighbor_info(cur), lfn->old_parent_eui64, MAC_ADDR_MODE_64_BIT)) {
        return;
    }
    lfn->release_tries++;
    lfn->release_sent = 0;
    lfn->release_acked = 0;
    ws_common_create_ll_address(ll, lfn->old_parent_eui64);
    ns_list_foreach(if_address_entry_t, addr, &cur->ip_addresses) {
        if (addr_is_ipv6_link_local(addr->address)) {
            continue;
        }
        memset(&aro, 0, sizeof(aro));
        aro.status = ARO_SUCCESS;
        aro.present = true;
        memcpy(aro.eui64, cur->mac, 8);
        aro.lifetime = 0;
        buffer_t *buf = icmpv6_build_ns(cur, ll, addr->address, true, false, &aro);
        if (buf) {
            // icmpv6_build_ns() routes before it sets the interface, so a
            // link-local destination would take our default route (the new
            // parent): on-link to the old parent
            ipv6_buffer_route_to(buf, ll, cur);
            buf->ack_receive_cb = ws_lfn_release_ack_cb;
            protocol_push(buf);
            lfn->release_sent++;
        }
    }
    ns_list_foreach(if_group_entry_t, g, &cur->ip_groups) {
        if (ws_lfn_group_registrable(g->group) && ws_lfn_mc_send(cur, ll, g->group, 0, true)) {
            lfn->release_sent++;
        }
    }
    tr_info("LFN: old parent %s released (try %u)", trace_array(lfn->old_parent_eui64, 8), lfn->release_tries);
    // Checked (and sent again if needed) later
    lfn->release_at_s = cur->ws_info->uptime + WS_LFN_RELEASE_RETRY_S;
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
        // Our groups (RFC 9685), with the same lifetime and refresh
        ns_list_foreach(if_group_entry_t, g, &cur->ip_groups) {
            if (ws_lfn_group_registrable(g->group)) {
                ws_lfn_mc_send(cur, ll_parent, g->group, cur->ws_info->lfn_cfg.registration_lifetime_min, false);
            }
        }
        // A parent left for this one: released once our route has moved.
        // Meanwhile it may still hold frames for us, which it would send in
        // our old windows: a frame to it now (an NS for its link-local
        // address) carries our new timing (LUTT, LUS) [I]
        if (cur->ws_info->lfn.release_at_s == UINT32_MAX) {
            uint8_t ll_old[16];
            ws_common_create_ll_address(ll_old, cur->ws_info->lfn.old_parent_eui64);
            buffer_t *ns = icmpv6_build_ns(cur, ll_old, NULL, true, false, NULL);
            if (ns) {
                ipv6_buffer_route_to(ns, ll_old, cur);
                protocol_push(ns);
            }
            cur->ws_info->lfn.release_at_s = cur->ws_info->uptime + cur->ws_info->lfn_cfg.parent_release_s +
                                             2 * ((cur->ws_info->lfn.listen_interval_ms + 999) / 1000);
            cur->ws_info->lfn.release_tries = 0;
        }
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
            lfn->uplink_fails = 0;
            lfn->searching = false;
            lfn->search_next_s = 0;
            lfn->check = false;
            lfn->check_next_s = 0;
            lfn->disc_slots = 0;
            lfn->release_at_s = 0;
            lfn->parent_rsl_x8 = 0;
            ws_lfn_network_configure(cur);
            // The receiver is off outside the listen windows
            cur->ws_info->rx_off_when_idle = true;

            ws_lfn_listen_apply(cur, cur->ws_info->lfn_cfg.listen_interval_ms);
            ws_bootstrap_fhss_activate(cur);
            // No PAN yet, so MLME-START did not synchronize the FHSS: the
            // listen schedule starts now
            ns_fhss_ws_lfn_listen_start(cur->ws_info->fhss_api);
            ws_bootstrap_configure_max_retries(cur, WS_MAX_FRAME_RETRIES_BOOTSTRAP);
            ws_bootstrap_configure_data_request_restart(cur, WS_CCA_REQUEST_RESTART_MAX, WS_TX_REQUEST_RESTART_MAX_BOOTSTRAP, cur->ws_info->lfn_cfg.restart_min_ms, cur->ws_info->lfn_cfg.restart_max_ms);
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
            // The LFN's restarts outlast a parent's asynchronous sweep
            ws_bootstrap_configure_data_request_restart(cur, WS_CCA_REQUEST_RESTART_MAX, WS_TX_REQUEST_RESTART_MAX, cur->ws_info->lfn_cfg.restart_min_ms, cur->ws_info->lfn_cfg.restart_max_ms);
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
            // The keys work: stored (non-volatile storage, when the
            // application gives one), as an FFN does once in a DODAG
            ws_pae_controller_nw_key_valid(cur, NULL);
            ws_pae_controller_frame_counter_store_now(cur);
            cur->ws_info->connected_time = cur->ws_info->uptime;
            ws_lfn_check_schedule(cur);
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
    if (lfn->release_at_s && lfn->release_at_s != UINT32_MAX) {
        uint32_t up = cur->ws_info->uptime;
        next = protocol_timer_next_min(next, lfn->release_at_s > up ? lfn->release_at_s - up : 1);
    }
    if (lfn->check_next_s && lfn->parent_known && cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE && !lfn->searching) {
        uint32_t up = cur->ws_info->uptime;
        next = protocol_timer_next_min(next, lfn->check_next_s > up ? lfn->check_next_s - up : 1);
    }
    if (lfn->parent_known && cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE && !lfn->searching) {
        // The parent search (see ws_bootstrap_lfn_seconds_timer)
        const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
        uint32_t up = cur->ws_info->uptime;
        uint32_t since = up - lfn->parent_rx_s;
        uint32_t at = 0;
        if (c->search_uplink_fails && lfn->uplink_fails >= c->search_uplink_fails) {
            at = 1;
        } else if (c->search_sync_periods) {
            uint32_t lim = ws_lfn_search_limit_s(cur);
            at = since < lim ? lim - since + 1 : 1;
        }
        if (at) {
            if (lfn->search_next_s > up && lfn->search_next_s - up > at) {
                at = lfn->search_next_s - up;
            }
            next = protocol_timer_next_min(next, at);
        }
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
    // The old parent's release
    if (lfn->release_at_s && lfn->release_at_s != UINT32_MAX && cur->ws_info->uptime >= lfn->release_at_s) {
        ws_lfn_old_parent_release(cur);
    }
    // Background check for a better parent (docs/LFN_STACK.md §9) [I]
    if (lfn->parent_known && cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE && !lfn->searching && !lfn->disc_decide_s &&
            lfn->check_next_s && cur->ws_info->uptime >= lfn->check_next_s) {
        tr_info("LFN: better-parent check (parent %s, %d dBm, routing cost %u)", trace_array(lfn->parent_eui64, 8),
                lfn->parent_rsl_x8 ? (int)((lfn->parent_rsl_x8 + 4) >> WS_RSL_SCALING) - 174 : 0, lfn->parent_cost);
        ws_lfn_check_schedule(cur);
        lfn->searching = true;
        lfn->check = true;
        lfn->disc_slots = cur->ws_info->lfn_cfg.parent_check_lnd_slots;
        ws_lfn_lpas_send(cur);
        if (!lfn->disc_decide_s) {
            // No discovery window (a broadcast window was open): in a second
            lfn->searching = false;
            lfn->check = false;
            lfn->disc_slots = 0;
            lfn->check_next_s = cur->ws_info->uptime + 1;
        }
    }
    // Parent change without loss: a discovery while the parent is kept, when
    // it has been silent for search_sync_periods sync periods or has missed
    // search_uplink_fails uplinks in a row [I]
    if (lfn->parent_known && cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE && !lfn->searching && !lfn->disc_decide_s &&
            cur->ws_info->uptime >= lfn->search_next_s) {
        const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
        uint32_t since = cur->ws_info->uptime - lfn->parent_rx_s;
        bool silent = c->search_sync_periods && since > ws_lfn_search_limit_s(cur);
        bool failing = c->search_uplink_fails && lfn->uplink_fails >= c->search_uplink_fails;
        if (silent || failing) {
            tr_info("LFN: parent search (%s), parent kept meanwhile", silent ? "no sync frame" : "uplinks failed");
            lfn->searching = true;
            // the next search one sync period later, if this one finds nobody
            lfn->search_next_s = cur->ws_info->uptime + ws_lfn_sync_period_s(cur);
            ws_lfn_lpas_send(cur);
            if (!lfn->disc_decide_s) {
                // No discovery window (a broadcast window was open): again
                // in a second
                lfn->searching = false;
                lfn->search_next_s = cur->ws_info->uptime + 1;
            }
        }
    }
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
