/*
 * Copyright (c) 2026, the mesh-emulator-rework authors.
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

/*
 * Wi-SUN FAN 1.1: the LFN parent side of the border router, and the LFN-side
 * LGTK checks (GS8 stage 3; docs/LFN_STACK.md).
 *
 * Parent: an LPAS creates the LFN child (its LUTT/LUS schedule, its US
 * channel plan) and is answered by a unicast LPA in the LFN's next window
 * (the MAC holds it), with an LTO-IE that snaps its interval to a multiple or
 * divisor of the LFN broadcast interval and spreads its offset; an LPCS is
 * answered by an LPC (LFN version, LGTK hash).
 *
 * LFN broadcast schedule (stage 4): while the border router has an LFN
 * child, the slot of every sync period carries a sync frame: an LTS, or a
 * broadcast LPC for maintain_parent_time sync periods after the schedule
 * starts and after each LGTK event, whose LPC goes in the next slot [M:
 * black-box items 32, 48, §5.2; S: wsbrd ws_mngt.c]. The FHSS holds these
 * frames for their slot and writes their LBT-IE.
 */

#include <string.h>
#include "nsconfig.h"
#include <stdint.h>
#include "ns_rand.h"
#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/common_functions.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack/mac/fhss_ws_extension.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/sw_mac.h"
#include "nanostack/mac/mac_mcps.h"
#include "6lowpan/mac/mpx_api.h"
#include "nanostack/ws_management_api.h"
#include "nwk_interface/protocol.h"
#include "service_libs/mac_neighbor_table/mac_neighbor_table.h"
#include "common_protocols/icmpv6.h"
#include "6lowpan/mac/mac_helper.h"
#include "6lowpan/ws/ws_common_defines.h"
#include "6lowpan/ws/ws_common.h"
#include "6lowpan/ws/ws_bootstrap.h"
#include "6lowpan/ws/ws_cfg_settings.h"
#include "6lowpan/ws/ws_bootstrap_6lbr.h"
#include "6lowpan/ws/ws_bootstrap_lfn.h"
#include "6lowpan/ws/ws_ie_lib.h"
#include "6lowpan/ws/ws_llc.h"
#include "6lowpan/ws/ws_neighbor_class.h"
#include "6lowpan/ws/ws_lfn_parent.h"
#include "core/ns_buffer.h"
#include "ipv6_stack/ipv6_routing_table.h"
#include "common_protocols/ipv6_constants.h"
#include "core/ns_address_internal.h"
#include "6lowpan/iphc_decode/iphc_compress.h"
#include "security/protocols/sec_prot_keys.h"
#include "6lowpan/ws/ws_pae_controller.h"

#ifdef HAVE_WS_VERSION_1_1

#define TRACE_GROUP "wslp"

/* Guard between LFN unicast slots and the LFN broadcast slot, and the step
 * of the offsets given in LTO-IEs (ms; the value wisun-br-linux uses) */
#define WS_LFN_LTO_GUARD_MS 300

/* Interval accepted for an LFN: its proposal, snapped to a multiple (above
 * the LFN broadcast interval) or a divisor (below it) of the LFN broadcast
 * interval within the LFN's NR-IE range, so that its slots keep their
 * position against the broadcast slots. Our arithmetic, after the rule
 * described in wisun-br-linux (ws_neigh.c) [S]. */
static uint32_t ws_lfn_adjusted_interval(uint32_t bc, uint32_t uc, uint32_t min, uint32_t max)
{
    uint32_t r, q;

    if (!bc || !uc || uc < min || uc > max) {
        return uc;
    }
    if (uc >= bc) {
        r = uc % bc;
        if (!r) {
            return uc;
        }
        if (uc + bc - r <= max) {
            return uc + bc - r;
        }
        if (uc - r >= min) {
            return uc - r;
        }
        return uc;
    }
    if (bc % uc == 0) {
        return uc;
    }
    // The nearest divisors of bc around uc: bc / q for q above and below bc / uc
    for (q = bc / uc + 1; q <= bc; q++) {
        if (bc % q == 0) {
            if (bc / q >= min) {
                return bc / q;
            }
            break;
        }
    }
    for (q = bc / uc; q >= 1; q--) {
        if (bc % q == 0) {
            if (bc / q <= max) {
                return bc / q;
            }
            break;
        }
    }
    return uc;
}

static uint32_t ws_lfn_offset(uint32_t interval, uint32_t bc)
{
    uint32_t max = (interval >= bc ? bc : interval);

    if (interval < 2 * WS_LFN_LTO_GUARD_MS || max < 2 * WS_LFN_LTO_GUARD_MS) {
        return 0;
    }
    return WS_LFN_LTO_GUARD_MS * rand_get_random_in_range(1, (max - WS_LFN_LTO_GUARD_MS) / WS_LFN_LTO_GUARD_MS);
}

static void ws_lfn_security_set(protocol_interface_info_entry_t *cur, asynch_request_t *req)
{
    req->security.SecurityLevel = mac_helper_default_security_level_get(cur);
    req->security.KeyIdMode = mac_helper_default_security_key_id_mode_get(cur);
    req->security.KeyIndex = mac_helper_default_key_index_get(cur);
}

static void ws_lfn_lpa_record(protocol_interface_info_entry_t *cur, const uint8_t *eui64, uint32_t window_ms);

static void ws_lfn_lpa_send(protocol_interface_info_entry_t *cur, const uint8_t *dst, const ws_nr_ie_t *nr, uint32_t proposed_ms, const ws_lnd_ie_t *lnd, uint32_t rx_timestamp, bool lto)
{
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    asynch_request_t req;

    memset(&req, 0, sizeof(req));
    req.message_type = WS_FT_LPA;
    req.unicast = true;
    memcpy(req.dst_address, dst, 8);
    if (lnd && lnd->discovery_slots && lnd->discovery_slot_time) {
        /* In a random slot k of the LFN's discovery window [S: wsbrd
           ws_mngt_lpa_schedule; M items 4-5], a quarter into it, on the LFN's
           unicast channel of slot number discovery_first_slot + k [M:
           black-box phase 2 analysis, every LPA of the phase 1 captures] */
        uint16_t k = rand_get_random_in_range(0, lnd->discovery_slots - 1);
        req.tx_at_set = true;
        req.tx_at = rx_timestamp + lnd->response_delay * 1000u + k * lnd->discovery_slot_time * 1000u +
                    lnd->discovery_slot_time * 250u;
        req.tx_slot_set = true;
        req.tx_slot = (uint16_t)(lnd->discovery_first_slot + k);
        tr_debug("LPA to %s in LND slot %u (channel slot %u)", trace_array(dst, 8), k, req.tx_slot);
    }
    req.wh_requested_ie_list.utt_ie = true;
    req.wh_requested_ie_list.bt_ie = true;
    req.wh_requested_ie_list.nr_ie = true;
    req.wh_requested_ie_list.panid_ie = true;
    req.wh_requested_ie_list.lbs_ie = true;
    req.wh_requested_ie_list.lbt_ie = true;
    req.wh_requested_ie_list.flus_ie = true;
    if (c->lto && lto) {
        uint32_t ivl = ws_lfn_adjusted_interval(c->bc_interval_ms, proposed_ms, nr->listen_interval_min, nr->listen_interval_max);
        req.wh_requested_ie_list.lto_ie = true;
        req.lto.adjusted_listening_interval = ivl;
        // The phase of its slots against our LFN broadcast slots; the FHSS
        // converts it to an offset against the LFN's slot at TX time
        req.lto.offset = ws_lfn_offset(ivl, c->bc_interval_ms);
        tr_info("LPA to %s: LTO phase %"PRIu32" ms, interval %"PRIu32" ms (proposed %"PRIu32")",
                trace_array(dst, 8), (uint32_t)req.lto.offset, ivl, proposed_ms);
    }
    // Our unicast schedule as FLUS-IE + LCP-IE (tag 0), no US-IE [S:
    // wisun-br-linux ws_mngt.c; M: Silicon Labs' LPA]
    req.wp_requested_nested_ie_list.lcp_ie = true;
    // Test hook (ws_test_lfn_lpa_unusable()): an LPA without BS-IE
    req.wp_requested_nested_ie_list.bs_ie = !cur->ws_info->lfn.test_lpa_no_bs;
    req.wp_requested_nested_ie_list.pan_ie = true;
    req.wp_requested_nested_ie_list.net_name_ie = true;
    // A router's routing cost is updated when it sends a PAN advertisement,
    // which can be rare: the PAN-IE of an LPA gives the current one
    ws_bootstrap_routing_cost_update(cur);
    ws_lfn_lpa_record(cur, dst, lnd ? (uint32_t)lnd->discovery_slots * lnd->discovery_slot_time : 0);
    if (cur->ws_info->lfn.test_lpa_drop) {
        // Test hook (ws_test_lfn_lpa_drop()): as if lost on the air, unacknowledged
        cur->ws_info->lfn.test_lpa_drop--;
        tr_info("LPA to %s dropped (test)", trace_array(dst, 8));
        return;
    }
    // Unsecured, as a PAN advertisement
    ws_llc_asynch_request(cur, &req);
}

static void ws_lfn_lpc_send(protocol_interface_info_entry_t *cur, const uint8_t *dst)
{
    asynch_request_t req;

    memset(&req, 0, sizeof(req));
    req.message_type = WS_FT_LPC;
    req.unicast = true;
    memcpy(req.dst_address, dst, 8);
    req.wh_requested_ie_list.utt_ie = true;
    req.wh_requested_ie_list.bt_ie = true;
    req.wh_requested_ie_list.lbs_ie = true;
    req.wh_requested_ie_list.lbt_ie = true;
    req.wp_requested_nested_ie_list.lfn_gtk_version_ie = true;
    ws_lfn_lgtk_security_set(cur, &req);
    ws_llc_asynch_request(cur, &req);
}

/* A sync frame for the next LFN broadcast slot: an LTS (UTT, BT, LBT), or an
 * LPC (also LBS, LFN version and LGTK hash), under the active LGTK */
static void ws_lfn_bc_send(protocol_interface_info_entry_t *cur, uint8_t message_type, uint16_t slot)
{
    asynch_request_t req;

    memset(&req, 0, sizeof(req));
    req.message_type = message_type;
    req.lfn_broadcast = true;
    req.wh_requested_ie_list.utt_ie = true;
    req.wh_requested_ie_list.bt_ie = true;
    req.wh_requested_ie_list.lbt_ie = true;
    if (message_type == WS_FT_LPC) {
        req.wh_requested_ie_list.lbs_ie = true;
        req.wp_requested_nested_ie_list.lfn_gtk_version_ie = true;
    }
    ws_lfn_lgtk_security_set(cur, &req);
    tr_debug("LFN broadcast slot %u: %s", slot, message_type == WS_FT_LPC ? "LPC" : "LTS");
    ws_llc_asynch_request(cur, &req);
}

/* One LPA per discovery: an LPAS sweep reaches us on several channels (and
 * a check sends two), and a second LPA (with a second LTO-IE) would move the
 * LFN's schedule twice in our view and once in its own (docs/LFN_STACK.md
 * §4). Two rules:
 * - no new LPA while one to the same LFN is still queued
 *   (ws_llc_lfn_lpa_pending(); wisun-br-linux v2.10.3 ws_mngt_lpas_analyze()
 *   has only this rule [S]);
 * - after an LPA the LFN acknowledged, its LPAS are ignored until the end of
 *   the discovery window that LPA answered (the LND-IE's slots x slot time,
 *   plus 1 s): the other LPAS of the same sweeps. A lost or unacknowledged
 *   LPA starts no hold-off, so the LFN's next sweep is answered. Before, a
 *   hold-off of 2 proposed intervals started before the LPA went out, and
 *   even when it was then dropped: under the Eco profile (300 s) a lost LPA
 *   delayed the join by about 10 min [I]. */
#define WS_LFN_LPA_RECENT 8
static struct {
    uint8_t eui64[8];
    uint32_t window_s;      /**< Hold-off length, armed when the LPA is acknowledged */
    uint32_t until_s;       /**< End of the hold-off (0: not armed) */
} ws_lfn_lpa_recent[WS_LFN_LPA_RECENT];

static bool ws_lfn_lpa_held_off(protocol_interface_info_entry_t *cur, const uint8_t *eui64)
{
    for (int i = 0; i < WS_LFN_LPA_RECENT; i++) {
        if (!memcmp(ws_lfn_lpa_recent[i].eui64, eui64, 8) && ws_lfn_lpa_recent[i].until_s &&
                (int32_t)(ws_lfn_lpa_recent[i].until_s - cur->ws_info->uptime) > 0) {
            return true;
        }
    }
    return false;
}

/* An LPA queued: its hold-off length, armed by its acknowledgement */
static void ws_lfn_lpa_record(protocol_interface_info_entry_t *cur, const uint8_t *eui64, uint32_t window_ms)
{
    uint32_t now = cur->ws_info->uptime;
    int slot = -1, oldest = 0;

    for (int i = 0; i < WS_LFN_LPA_RECENT; i++) {
        if (!memcmp(ws_lfn_lpa_recent[i].eui64, eui64, 8)) {
            slot = i;
            break;
        }
        if (slot < 0 && (!ws_lfn_lpa_recent[i].until_s || (int32_t)(ws_lfn_lpa_recent[i].until_s - now) <= 0)) {
            slot = i;
        }
        if (ws_lfn_lpa_recent[i].until_s < ws_lfn_lpa_recent[oldest].until_s) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }
    memcpy(ws_lfn_lpa_recent[slot].eui64, eui64, 8);
    ws_lfn_lpa_recent[slot].window_s = (window_ms + 999) / 1000 + 1;
    ws_lfn_lpa_recent[slot].until_s = 0;
}

void ws_lfn_parent_lpa_confirm(protocol_interface_info_entry_t *cur, const uint8_t eui64[8], bool acked)
{
    for (int i = 0; i < WS_LFN_LPA_RECENT; i++) {
        if (!memcmp(ws_lfn_lpa_recent[i].eui64, eui64, 8)) {
            ws_lfn_lpa_recent[i].until_s = acked ? cur->ws_info->uptime + ws_lfn_lpa_recent[i].window_s : 0;
            return;
        }
    }
}

static bool ws_lfn_is_child(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *n, ws_neighbor_class_entry_t **wsn)
{
    ws_neighbor_class_entry_t *w = ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, n->index);
    if (!w || !w->fhss_data.lfn_uc_timing_info.listen_interval_ms) {
        return false;
    }
    if (wsn) {
        *wsn = w;
    }
    return true;
}

/* Keep an LFN child: it is silent for long, so no FFN timeouts or probes.
 * An LPAS makes a candidate child for WS_LFN_CANDIDATE_LIFETIME_S (another
 * parent may be chosen); the LFN's LPCS and its secured frames make it a
 * child for twice its registration lifetime [I]. */
#define WS_LFN_CANDIDATE_LIFETIME_S 600
/* An LFN child that deregistered (moved to another parent) */
#define WS_LFN_RELEASED_LIFETIME_S 60
static void ws_lfn_child_refresh(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *n, bool chosen)
{
    uint32_t life = (uint32_t)cur->ws_info->lfn_cfg.registration_lifetime_min * 60 * 2;

    if (!chosen) {
        if (n->link_lifetime > WS_LFN_CANDIDATE_LIFETIME_S && n->lifetime > WS_LFN_CANDIDATE_LIFETIME_S) {
            // already a child: an LPAS (a rediscovery) does not shorten it
            return;
        }
        life = WS_LFN_CANDIDATE_LIFETIME_S;
    }

    n->ffd_device = false;
    n->link_lifetime = life;
    n->lifetime = life;
}

void ws_lfn_parent_frame_rx(protocol_interface_info_entry_t *cur, const uint8_t *src64)
{
    mac_neighbor_table_entry_t *n;

    if (!cur->ws_info->lfn_parent) {
        return;
    }
    n = mac_neighbor_table_address_discover(mac_neighbor_info(cur), src64, MAC_ADDR_MODE_64_BIT);
    // Not a child that released us (its last frames, the deregistrations,
    // do not keep it): its LPAS or LPCS makes it a child again
    if (n && ws_lfn_is_child(cur, n, NULL) && n->link_lifetime != WS_LFN_RELEASED_LIFETIME_S) {
        ws_lfn_child_refresh(cur, n, true);
    }
}

/* An FFN router (GS8 stage 5): the LGTK it secures its LFN frames with. The
 * active one the border router announces once it holds it; until then the
 * one it used last (its LFN children have no newer one either: they fetch it
 * through this router); -1 if it holds none. */
static int8_t ws_lfn_parent_router_lgtk(protocol_interface_info_entry_t *cur)
{
    ws_lfn_state_t *st = &cur->ws_info->lfn;
    uint8_t active = cur->ws_info->lfngtk.active_key_index;

    if (cur->ws_info->lfngtk.lfn_version_learned && ws_pae_controller_lgtk_is_set(cur, active)) {
        st->tx_lgtk = active;
        st->tx_lgtk_valid = true;
        return active;
    }
    if (st->tx_lgtk_valid && ws_pae_controller_lgtk_is_set(cur, st->tx_lgtk)) {
        return st->tx_lgtk;
    }
    for (uint8_t i = 0; i < LGTK_NUM; i++) {
        if (ws_pae_controller_lgtk_is_set(cur, i)) {
            return i;
        }
    }
    return -1;
}

/* An FFN router parents LFNs once it is operational and holds an LGTK; the
 * border router always */
static bool ws_lfn_parent_ready(protocol_interface_info_entry_t *cur)
{
    if (wisun_mode_border_router(cur)) {
        return true;
    }
    return cur->nwk_bootstrap_state == ER_BOOTSTRAP_DONE && ws_lfn_parent_router_lgtk(cur) >= 0;
}

bool ws_lfn_parent_asynch_ind(protocol_interface_info_entry_t *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext, uint8_t message_type)
{
    llc_neighbour_req_t ni;
    ws_us_ie_t us;
    ws_nr_ie_t nr;
    ws_lus_ie_t lus;

    if (message_type != WS_FT_LPAS && message_type != WS_FT_LPCS) {
        return false;
    }
    if (!cur->ws_info->lfn_parent || cur->ws_info->lfn.test_silent || !ws_lfn_parent_ready(cur)) {
        return true;
    }
    if (message_type == WS_FT_LPAS) {
        // Ours if the network name matches; LUTT, LUS, NR and the LCP-IE of
        // the LUS-IE's tag (or a US-IE, our LFNs before the black-box phase 2
        // work) needed [S: wisun-br-linux ws_mngt.c]
        const char *why = NULL;
        if (!ws_bootstrap_network_name_matches(ie_ext, cur->ws_info->cfg->gen.network_name)) {
            why = "network name";
        } else if (!ws_wh_nr_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &nr) || nr.node_role != WS_NR_ROLE_LFN) {
            why = "NR-IE";
        } else if (!ws_wh_lus_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lus)) {
            why = "LUS-IE";
        } else if (!(lus.channel_plan_tag != WS_LCP_TAG_CURRENT &&
                     ws_wp_nested_lcp_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, lus.channel_plan_tag, 0, &us)) &&
                   !ws_wp_nested_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &us)) {
            why = "LCP-IE";
        } else if (!ws_bootstrap_validate_channel_plan(&us, cur) || !ws_bootstrap_validate_channel_function(&us, NULL)) {
            why = "channel plan";
        }
        if (why) {
            tr_debug("LPAS from %s ignored (%s)", trace_array(data->SrcAddr, 8), why);
            return true;
        }
        if (ws_lfn_lpa_held_off(cur, data->SrcAddr)) {
            tr_debug("LPAS from %s: LPA already sent", trace_array(data->SrcAddr, 8));
            return true;
        }
        if (!ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, true)) {

            tr_warn("LPAS: no room for LFN %s", trace_array(data->SrcAddr, 8));
            return true;
        }
        /* Our child already (its LPCS or secured frames made it one): an LPAS
           of a background check for a better parent. No LTO-IE then: its
           slots stay where they are, in its view and ours, whether or not
           our LPA and its ACK get through [I] */
        bool known_child = ni.neighbor->trusted_device && ws_lfn_is_child(cur, ni.neighbor, NULL) &&
                           ni.neighbor->link_lifetime > WS_LFN_CANDIDATE_LIFETIME_S;
        ws_neighbor_class_neighbor_unicast_schedule_set(cur, ni.ws_neighbor, &us, &cur->ws_info->hopping_schedule, data->SrcAddr);
        if (!ws_llc_lfn_timing_update(cur, ni.ws_neighbor, ie_ext, data->timestamp)) {
            return true;
        }
        ws_lfn_child_refresh(cur, ni.neighbor, false);
        if (ws_llc_lfn_lpa_pending(cur, data->SrcAddr)) {
            /* The LPA of an earlier sweep is still waiting for a window: a
               second one would carry a second LTO-IE, which the LFN ignores
               once it has a parent but which would move its schedule in our
               view when acknowledged */
            tr_debug("LPAS from %s: an LPA is still pending", trace_array(data->SrcAddr, 8));
            return true;
        }
        tr_info("LPAS from LFN %s: interval %"PRIu32" ms", trace_array(data->SrcAddr, 8), (uint32_t)lus.listen_interval);
        ws_lnd_ie_t lnd;
        bool has_lnd = ws_wh_lnd_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lnd);
        ws_lfn_lpa_send(cur, data->SrcAddr, &nr, lus.listen_interval, has_lnd ? &lnd : NULL, data->timestamp, !known_child);
        return true;
    }
    /* LPCS: from a known LFN. Silicon Labs' LFN sends it unsecured (with its
       network name) and our LFN secured [M: black-box phase 1] */
    if (!ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, false) ||
            (!data->Key.SecurityLevel && !ws_bootstrap_network_name_matches(ie_ext, cur->ws_info->cfg->gen.network_name))) {
        return true;
    }
    if ((ws_wh_lus_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lus) && lus.channel_plan_tag != WS_LCP_TAG_CURRENT &&
            ws_wp_nested_lcp_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, lus.channel_plan_tag, 0, &us)) ||
            ws_wp_nested_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &us)) {
        ws_neighbor_class_neighbor_unicast_schedule_set(cur, ni.ws_neighbor, &us, &cur->ws_info->hopping_schedule, data->SrcAddr);
    }
    ws_llc_lfn_timing_update(cur, ni.ws_neighbor, ie_ext, data->timestamp);
    ws_lfn_child_refresh(cur, ni.neighbor, true);
    if (data->Key.SecurityLevel) {
        // An unsecured LPCS does not make it trusted: its secured frames do
        mac_neighbor_table_trusted_neighbor(mac_neighbor_info(cur), ni.neighbor, true);
    }
    tr_info("LPCS from LFN %s: LPC", trace_array(data->SrcAddr, 8));
    ws_lfn_lpc_send(cur, data->SrcAddr);
    return true;
}

int ws_lfn_parent_children_get(protocol_interface_info_entry_t *cur, ws_lfn_child_info_t *table, int size)
{
    int n = 0;
    ns_list_foreach(mac_neighbor_table_entry_t, e, &mac_neighbor_info(cur)->neighbour_list) {
        ws_neighbor_class_entry_t *w;
        if (n >= size || !ws_lfn_is_child(cur, e, &w)) {
            continue;
        }
        memcpy(table[n].eui64, e->mac64, 8);
        table[n].listen_interval_ms = w->fhss_data.lfn_uc_timing_info.listen_interval_ms;
        table[n].last_heard_s = e->link_lifetime >= e->lifetime ? e->link_lifetime - e->lifetime : 0;
        table[n].state = e->link_lifetime == WS_LFN_RELEASED_LIFETIME_S ? 2 :
                         e->link_lifetime > WS_LFN_CANDIDATE_LIFETIME_S ? 1 : 0;
        n++;
    }
    return n;
}

/* An LFN child that needs our sync frames: not one that released us */
static bool ws_lfn_has_child(protocol_interface_info_entry_t *cur)
{
    ns_list_foreach(mac_neighbor_table_entry_t, n, &mac_neighbor_info(cur)->neighbour_list) {
        if (ws_lfn_is_child(cur, n, NULL) && n->link_lifetime != WS_LFN_RELEASED_LIFETIME_S) {
            return true;
        }
    }
    return false;
}

/* Queue the sync frame of the next LFN broadcast slot once it is less than
 * WS_LFN_BC_QUEUE_AHEAD_US away (this timer runs every second); the MAC holds
 * it until the slot. */
#define WS_LFN_BC_QUEUE_AHEAD_US 2000000

void ws_lfn_parent_seconds_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    ws_lfn_state_t *st = &cur->ws_info->lfn;
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    uint16_t slot;
    uint32_t start;

    (void)seconds;
    if (!cur->ws_info->lfn_parent) {
        return;
    }
    /* Keep every LFN child's slots on the 32-bit µs clock, whether or not we
       send to it: its LUTT reference is re-anchored once it is 2^30 µs old
       (LFN_MAC.md §4). Without this, a child silent and without downlinks
       for more than 2^31 µs (35.8 min) was reached on wrong slots */
    ns_list_foreach(mac_neighbor_table_entry_t, n, &mac_neighbor_info(cur)->neighbour_list) {
        if (ws_lfn_is_child(cur, n, NULL)) {
            ns_fhss_ws_lfn_neighbor_refresh(cur->ws_info->fhss_api, n->mac64);
        }
    }
    if (st->test_silent || !ws_lfn_parent_ready(cur)) {
        return;
    }
    if (!ws_lfn_has_child(cur)) {
        // No sync frames without an LFN child [M item 32]; the first ones
        // after a child appears are LPCs
        st->bc_lpc_left = c->maintain_parent_time;
        st->lpc_pending = 0;
        return;
    }
    if (ns_fhss_ws_lfn_bc_next(cur->ws_info->fhss_api, &slot, &start)) {
        return;
    }
    if ((int32_t)(start - ns_sw_mac_read_current_timestamp(cur->mac_api)) > WS_LFN_BC_QUEUE_AHEAD_US ||
            (st->bc_slot_queued_valid && st->bc_slot_queued == slot)) {
        return;
    }
    if (st->lpc_pending) {
        // An LGTK event: an LPC in the next slot, then LPCs instead of LTSs
        // for maintain_parent_time sync periods [M item 48]
        st->lpc_pending = 0;
        st->bc_lpc_left = c->maintain_parent_time;
        ws_lfn_bc_send(cur, WS_FT_LPC, slot);
    } else if (!(slot % c->bc_sync_period)) {
        if (st->bc_lpc_left) {
            st->bc_lpc_left--;
            ws_lfn_bc_send(cur, WS_FT_LPC, slot);
        } else {
            ws_lfn_bc_send(cur, WS_FT_LTS, slot);
        }
    } else {
        return;
    }
    st->bc_slot_queued = slot;
    st->bc_slot_queued_valid = true;
}

/* RFC 9685 multicast registrations of LFN children (EARO with P = 1,
 * icmpv6.c): the group, the LFN, and the end of the registration */
/* Sized for about 4 groups for each of 8 LFN children; an application with
 * more children or groups raises it at build time. A registration that does
 * not fit is refused with EARO status 2 (Neighbor Cache Full, RFC 8505 §4.1)
 * so that the LFN knows; entries of LFNs that are no longer our neighbours
 * are free. */
#ifndef WS_LFN_MC_REG_MAX
#define WS_LFN_MC_REG_MAX 32
#endif
static struct {
    uint8_t eui64[8];
    uint8_t group[16];
    uint32_t until_s;
} ws_lfn_mc_reg[WS_LFN_MC_REG_MAX];

uint8_t ws_lfn_parent_mc_register(protocol_interface_info_entry_t *cur, const uint8_t eui64[8], const uint8_t group[16], uint16_t lifetime_min)
{
    uint32_t now = cur->ws_info->uptime;
    int slot = -1;

    for (int i = 0; i < WS_LFN_MC_REG_MAX; i++) {
        bool live = (int32_t)(ws_lfn_mc_reg[i].until_s - now) > 0;
        if (live && !memcmp(ws_lfn_mc_reg[i].eui64, eui64, 8) && !memcmp(ws_lfn_mc_reg[i].group, group, 16)) {
            slot = i;
            break;
        }
        if (live && memcmp(ws_lfn_mc_reg[i].eui64, eui64, 8) &&
                !mac_neighbor_table_address_discover(mac_neighbor_info(cur), ws_lfn_mc_reg[i].eui64, MAC_ADDR_MODE_64_BIT)) {
            // The LFN left us (its neighbour entry was removed): free
            ws_lfn_mc_reg[i].until_s = now;
            live = false;
        }
        if (!live && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        if (!lifetime_min) {
            return ARO_SUCCESS;     // nothing to remove
        }
        tr_warn("LFN multicast registrations full: %s from %s refused (status 2)", trace_ipv6(group), trace_array(eui64, 8));
        return ARO_FULL;
    }
    memcpy(ws_lfn_mc_reg[slot].eui64, eui64, 8);
    memcpy(ws_lfn_mc_reg[slot].group, group, 16);
    ws_lfn_mc_reg[slot].until_s = lifetime_min ? now + (uint32_t)lifetime_min * 60 : now;
    return ARO_SUCCESS;
}

/* A child removed (neighbour table): its registrations are freed */
void ws_lfn_parent_child_removed(protocol_interface_info_entry_t *cur, const uint8_t eui64[8])
{
    for (int i = 0; i < WS_LFN_MC_REG_MAX; i++) {
        if (!memcmp(ws_lfn_mc_reg[i].eui64, eui64, 8)) {
            ws_lfn_mc_reg[i].until_s = cur->ws_info->uptime;
            memset(ws_lfn_mc_reg[i].eui64, 0, 8);
        }
    }
}

static bool ws_lfn_mc_registered(protocol_interface_info_entry_t *cur, const uint8_t eui64[8], const uint8_t *group)
{
    for (int i = 0; i < WS_LFN_MC_REG_MAX; i++) {
        if ((int32_t)(ws_lfn_mc_reg[i].until_s - cur->ws_info->uptime) > 0 &&
                !memcmp(ws_lfn_mc_reg[i].eui64, eui64, 8) && !memcmp(ws_lfn_mc_reg[i].group, group, 16)) {
            return true;
        }
    }
    return false;
}

/* Remove a hop-by-hop header that holds only the MPL option and padding: an
 * LFN is not an MPL forwarder, and the option type (0x6d) tells a node that
 * does not know it to discard the packet [S: RFC 7731; wsbrd sends its
 * copies without it, black-box item 41] */
static void ws_lfn_mc_strip_mpl(buffer_t *buf)
{
    uint8_t *ip = buffer_data_pointer(buf);
    uint16_t len = buffer_data_length(buf);

    if (len < 48 || ip[6] != IPV6_NH_HOP_BY_HOP) {
        return;
    }
    uint8_t *hbh = ip + 40;
    uint16_t hbh_len = (hbh[1] + 1) * 8;
    if (40 + hbh_len > len) {
        return;
    }
    for (uint16_t i = 2; i < hbh_len;) {
        if (hbh[i] == 0) {                      // Pad1
            i++;
        } else if (hbh[i] == 1 || hbh[i] == 0x6d) {   // PadN, MPL
            i += 2 + hbh[i + 1];
        } else {
            return;                             // another option: keep it all
        }
    }
    ip[6] = hbh[0];
    common_write_16_bit(common_read_16_bit(ip + 4) - hbh_len, ip + 4);
    memmove(ip + hbh_len, ip, 40);
    buffer_data_strip_header(buf, hbh_len);
}

/* A multicast from this parent: one MAC-unicast copy to each LFN child that
 * listens to the group, sent in its listen window, IPv6 destination
 * unchanged (black-box items 41-42): ff02::1 to every child, other groups to
 * the children that registered them (RFC 9685) */
/* An MPL tunnel (outer destination ff03::fc, hop-by-hop MPL option, then
 * the IPv6 packet to the group, RFC 7731): the length of the outer part, 0
 * for another packet */
static uint16_t ws_lfn_mc_tunnel_len(buffer_t *buf)
{
    const uint8_t *ip = buffer_data_pointer(buf);
    uint16_t len = buffer_data_length(buf);

    if (len < 48 || memcmp(ip + 24, ADDR_ALL_MPL_FORWARDERS, 16) || ip[6] != IPV6_NH_HOP_BY_HOP) {
        return 0;
    }
    uint16_t hbh_len = (ip[41] + 1) * 8;
    if (ip[40] != IPV6_NH_IPV6 || 40 + hbh_len + 40 > len) {
        return 0;
    }
    return 40 + hbh_len;
}

/* A hop-by-hop header right after the IPv6 header holding an MPL option */
static bool ws_lfn_mc_has_mpl(buffer_t *buf)
{
    const uint8_t *ip = buffer_data_pointer(buf);
    uint16_t len = buffer_data_length(buf);

    if (len < 48 || ip[6] != IPV6_NH_HOP_BY_HOP) {
        return false;
    }
    uint16_t hbh_len = (ip[41] + 1) * 8;
    for (uint16_t i = 42; i + 1 < 40 + hbh_len && i + 1 < len;) {
        if (ip[i] == 0) {
            i++;
        } else if (ip[i] == 0x6d) {
            return true;
        } else {
            i += 2 + ip[i + 1];
        }
    }
    return false;
}

static void ws_lfn_mc_copy(protocol_interface_info_entry_t *cur, buffer_t *buf, bool stable_only, uint_fast16_t max_iphc_size, bool from_mpl)
{
    // A group's packet tunnelled in MPL: the inner packet is the one copied
    uint16_t outer = ws_lfn_mc_tunnel_len(buf);
    const uint8_t *ip_dst = buffer_data_pointer(buf) + outer + 24;
    bool all_nodes = !memcmp(ip_dst, ADDR_LINK_LOCAL_ALL_NODES, 16);

    ns_list_foreach(mac_neighbor_table_entry_t, n, &mac_neighbor_info(cur)->neighbour_list) {
        if (!n->trusted_device || !ws_lfn_is_child(cur, n, NULL) ||
                (!all_nodes && !ws_lfn_mc_registered(cur, n->mac64, ip_dst))) {
            continue;
        }
        buffer_t *copy = buffer_clone(buf);
        if (!copy) {
            return;
        }
        if (outer) {
            buffer_data_strip_header(copy, outer);
        }
        ws_lfn_mc_strip_mpl(copy);
        if (from_mpl) {
            // Taken from MPL (received or seeded), not from lowpan_down():
            // our MAC address as the source, fresh link-layer options
            memset(&copy->link_specific, 0, sizeof(copy->link_specific));
            copy->link_specific.ieee802_15_4.useDefaultPanId = true;
            copy->src_sa.addr_type = ADDR_802_15_4_LONG;
            if (!mac_helper_write_our_addr(cur, &copy->src_sa)) {
                buffer_free(copy);
                continue;
            }
            copy->interface = cur;
            copy->route = NULL;
        }
        copy->dst_sa.addr_type = ADDR_802_15_4_LONG;
        common_write_16_bit(cur->mac_parameters.pan_id, copy->dst_sa.address);
        memcpy(copy->dst_sa.address + 2, n->mac64, 8);
        copy->link_specific.ieee802_15_4.requestAck = true;
        copy = iphc_compress(&cur->lowpan_contexts, copy, max_iphc_size, stable_only);
        if (!copy) {
            continue;
        }
        copy->info = (buffer_info_t)(B_FROM_IPV6_TXRX | B_TO_MAC | B_DIR_DOWN);
        tr_debug("multicast copy to LFN %s", trace_array(n->mac64, 8));
        protocol_push(copy);
    }
}

/* lowpan_down(): a multicast leaving this parent. A packet carried by MPL
 * (tunnelled, or with an MPL option) is copied once, when MPL accepts it
 * (ws_lfn_parent_mpl_copy()): MPL transmits it again on Trickle timer
 * expirations, or not at all when it hears enough copies (k) */
void ws_lfn_parent_multicast_copy(protocol_interface_info_entry_t *cur, buffer_t *buf, bool stable_only, uint_fast16_t max_iphc_size)
{
    if (!cur->ws_info->lfn_parent || ws_lfn_mc_tunnel_len(buf) || ws_lfn_mc_has_mpl(buf)) {
        return;
    }
    ws_lfn_mc_copy(cur, buf, stable_only, max_iphc_size, false);
}

/* MPL accepted a new data message (received from a neighbour, or seeded
 * here): one copy to each LFN child that registered its group (RFC 9685),
 * also when this node never forwards it */
void ws_lfn_parent_mpl_copy(protocol_interface_info_entry_t *cur, buffer_t *buf)
{
    if (!cur || !ws_info(cur) || !cur->ws_info->lfn_parent || !ws_lfn_parent_ready(cur)) {
        return;
    }
    uint16_t overhead = mac_helper_frame_overhead(cur, buf);
    ws_lfn_mc_copy(cur, buf, true, mac_helper_max_payload_size(cur, overhead) - 4, true);
}

/* An LFN child ended its registrations (ARO lifetime 0: it moved to another
 * parent): its group registrations end, and it stays a child only for
 * WS_LFN_RELEASED_LIFETIME_S, so that this parent stops its sync frames and
 * copies to it [I] */
void ws_lfn_parent_child_release(protocol_interface_info_entry_t *cur, const uint8_t eui64[8])
{
    if (!cur || !ws_info(cur) || !cur->ws_info->lfn_parent) {
        return;
    }
    mac_neighbor_table_entry_t *n = mac_neighbor_table_address_discover(mac_neighbor_info(cur), eui64, MAC_ADDR_MODE_64_BIT);
    if (!n || !ws_lfn_is_child(cur, n, NULL)) {
        return;
    }
    // Another address still registered (the deregistered one is now tentative)
    ns_list_foreach(ipv6_neighbour_t, e, &cur->ipv6_neighbour_cache.list) {
        if (e->type == IP_NEIGHBOUR_REGISTERED && !memcmp(ipv6_neighbour_eui64(&cur->ipv6_neighbour_cache, e), eui64, 8)) {
            return;
        }
    }
    for (int i = 0; i < WS_LFN_MC_REG_MAX; i++) {
        if (!memcmp(ws_lfn_mc_reg[i].eui64, eui64, 8)) {
            ws_lfn_mc_reg[i].until_s = cur->ws_info->uptime;
        }
    }
    /* Kept WS_LFN_RELEASED_LIFETIME_S, and at least two of its listen
       intervals, to deliver the frames still queued for it (the LFN listens
       in the same slots, which its last frames gave us) */
    ws_neighbor_class_entry_t *w = NULL;
    ws_lfn_is_child(cur, n, &w);
    uint32_t keep = 2 * ((w->fhss_data.lfn_uc_timing_info.listen_interval_ms + 999) / 1000) + 10;
    if (keep < WS_LFN_RELEASED_LIFETIME_S) {
        keep = WS_LFN_RELEASED_LIFETIME_S;
    }
    if (n->lifetime > keep) {
        n->lifetime = keep;
    }
    n->link_lifetime = WS_LFN_RELEASED_LIFETIME_S;
    tr_info("LFN child %s released its registrations", trace_array(eui64, 8));
}

bool ws_lfn_parent_neighbor_keep(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *n)
{
    return cur->ws_info->lfn_parent && ws_lfn_is_child(cur, n, NULL);
}

/* ---- LGTK (docs/LFN_STACK.md §8) */

/* Border router: the key index of the active LGTK (5-7), 0 if none */
static uint8_t ws_lfn_parent_lgtk_key_index(protocol_interface_info_entry_t *cur)
{
    ws_lfn_lgtk_t *g = &cur->ws_info->lfngtk;
    if (!cur->ws_info->lfn_parent || !(g->active_hash_1 || g->active_hash_2 || g->active_hash_3)) {
        return 0;
    }
    if (!wisun_mode_border_router(cur)) {
        int8_t i = ws_lfn_parent_router_lgtk(cur);
        return i < 0 ? 0 : WS_LGTK_KEY_INDEX_FIRST + i;
    }
    return WS_LGTK_KEY_INDEX_FIRST + g->active_key_index;
}

/* Frames to LFN children (LPC) use the active LGTK */
void ws_lfn_lgtk_security_set(protocol_interface_info_entry_t *cur, asynch_request_t *req)
{
    ws_lfn_security_set(cur, req);
    uint8_t k = ws_lfn_parent_lgtk_key_index(cur);
    if (k) {
        req->security.KeyIndex = k;
    }
}

/* Silicon Labs' parents put an LBT-IE in every frame to an LFN (data,
 * EAPOL, ACK), and their LFN drops back to PAN selection when the parent's
 * frames lack it [M: black-box phases 1 and 3] */
bool ws_lfn_parent_dst_is_lfn(protocol_interface_info_entry_t *cur, const uint8_t *dst64)
{
    if (!cur->ws_info->lfn_parent || !dst64) {
        return false;
    }
    mac_neighbor_table_entry_t *n = mac_neighbor_table_address_discover(mac_neighbor_info(cur), dst64, MAC_ADDR_MODE_64_BIT);
    return n && ws_lfn_is_child(cur, n, NULL);
}

uint8_t ws_lfn_parent_tx_key_index(protocol_interface_info_entry_t *cur, const uint8_t *dst64)
{
    if (!cur->ws_info->lfn_parent) {
        return 0;
    }
    mac_neighbor_table_entry_t *n = mac_neighbor_table_address_discover(mac_neighbor_info(cur), dst64, MAC_ADDR_MODE_64_BIT);
    if (!n || !ws_lfn_is_child(cur, n, NULL)) {
        return 0;
    }
    return ws_lfn_parent_lgtk_key_index(cur);
}

/* Border router: an LGTK event (installed, activated, expired). The LGTK
 * hash and active index for the LPC (LGTKHASH-IE), the LFN version up by
 * one, and a broadcast LPC in the next LFN broadcast slot (black-box item
 * 48: each event bumps the LFN version and triggers an LPC). */
void ws_lfn_parent_lgtk_update(protocol_interface_info_entry_t *cur, sec_prot_gtk_keys_t *lgtks, int8_t active_index, const char *event)
{
    ws_lfn_lgtk_t *g = &cur->ws_info->lfngtk;
    bool set[LGTK_NUM];

    memset(g->lgtkhash, 0, sizeof(g->lgtkhash));
    for (uint8_t i = 0; i < LGTK_NUM; i++) {
        uint8_t *lgtk = sec_prot_keys_gtk_get(lgtks, i);
        set[i] = lgtk != NULL;
        if (lgtk) {
            sec_prot_keys_gtk_hash_generate(lgtk, g->lgtkhash + i * GTK_HASH_LEN);
        }
    }
    g->active_hash_1 = set[0];
    g->active_hash_2 = set[1];
    g->active_hash_3 = set[2];
    if (active_index >= 0) {
        g->active_key_index = active_index;
    }
    g->lfn_version++;
    g->lfn_version_learned = true;
    cur->ws_info->lfn.lpc_pending = 1;
    tr_info("LGTK %s: LFN version %u, LGTKs %s%s%s, active index %d", event, g->lfn_version,
            set[0] ? "0" : "-", set[1] ? "1" : "-", set[2] ? "2" : "-", active_index);
}

/* LFN: the LGTK hash of an LPC from the parent. LGTKs it no longer lists are
 * removed; a listed one we lack is fetched lgtk_max_mismatch_s later with a
 * key request (group-key handshake), as Silicon Labs' LFN does (black-box
 * items 49-50) [M, I: a fixed delay from the first LPC that shows it]. */
void ws_lfn_parent_lgtk_hash_check(protocol_interface_info_entry_t *cur, const struct mcps_data_ie_list *ie_ext)
{
#ifdef HAVE_WS_HOST
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;
    ws_lgtkhash_ie_t h;
    uint8_t hashes[3 * GTK_HASH_LEN];

    if (!ws_wp_nested_lgtk_hash_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &h)) {
        return;
    }
    memset(hashes, 0, sizeof(hashes));
    if (h.lgtk0) {
        memcpy(hashes, h.lgtk0_hash, GTK_HASH_LEN);
    }
    if (h.lgtk1) {
        memcpy(hashes + GTK_HASH_LEN, h.lgtk1_hash, GTK_HASH_LEN);
    }
    if (h.lgtk2) {
        memcpy(hashes + 2 * GTK_HASH_LEN, h.lgtk2_hash, GTK_HASH_LEN);
    }
    int8_t mismatch = ws_pae_controller_lgtk_hash_update(cur, hashes, h.lgtk0 | h.lgtk1 << 1 | h.lgtk2 << 2);
    if (mismatch == GTK_HASH_MISMATCH) {
        if (!lfn->lgtk_mismatch_timer_s && !lfn->lgtk_request_sent) {
            lfn->lgtk_mismatch_timer_s = cur->ws_info->lfn_cfg.lgtk_max_mismatch_s ? cur->ws_info->lfn_cfg.lgtk_max_mismatch_s : 1;
            tr_info("LFN: LGTK hash mismatch (active index %u), key request in %u s", h.active_lgtk_index, lfn->lgtk_mismatch_timer_s);
        }
    } else if (mismatch >= 0) {
        if (lfn->lgtk_mismatch_timer_s || lfn->lgtk_request_sent) {
            tr_info("LFN: LGTKs match the parent's hash");
        }
        lfn->lgtk_mismatch_timer_s = 0;
        lfn->lgtk_request_sent = false;
    }
#else
    (void)cur;
    (void)ie_ext;
#endif
}

void ws_lfn_parent_lgtk_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
#ifdef HAVE_WS_HOST
    ws_lfn_state_t *lfn = &cur->ws_info->lfn;

    if (!lfn->lgtk_mismatch_timer_s) {
        return;
    }
    if (lfn->lgtk_mismatch_timer_s > seconds) {
        lfn->lgtk_mismatch_timer_s -= seconds;
        return;
    }
    lfn->lgtk_mismatch_timer_s = 0;
    lfn->lgtk_request_sent = true;
    tr_info("LFN: LGTK key request to %s", trace_array(lfn->parent_eui64, 8));
    ws_pae_controller_lgtk_key_request(cur);
#else
    (void)cur;
    (void)seconds;
#endif
}

#endif /* HAVE_WS_VERSION_1_1 */
