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
 * answered by an LPC (LFN version, LGTK hash). Until the LFN broadcast
 * schedule exists (stage 4), the sync frames are unicast LPCs to each LFN
 * child every sync period, and an LGTK event sends one LPC to each child at
 * once.
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
#include "nanostack/mac/mac_mcps.h"
#include "6lowpan/mac/mpx_api.h"
#include "nanostack/ws_management_api.h"
#include "nwk_interface/protocol.h"
#include "service_libs/mac_neighbor_table/mac_neighbor_table.h"
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
#include "core/ns_address_internal.h"
#include "6lowpan/iphc_decode/iphc_compress.h"

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

static void ws_lfn_lpa_send(protocol_interface_info_entry_t *cur, const uint8_t *dst, const ws_nr_ie_t *nr, uint32_t proposed_ms)
{
    const ws_lfn_settings_t *c = &cur->ws_info->lfn_cfg;
    asynch_request_t req;

    memset(&req, 0, sizeof(req));
    req.message_type = WS_FT_LPA;
    req.unicast = true;
    memcpy(req.dst_address, dst, 8);
    req.wh_requested_ie_list.utt_ie = true;
    req.wh_requested_ie_list.bt_ie = true;
    req.wh_requested_ie_list.nr_ie = true;
    req.wh_requested_ie_list.panid_ie = true;
    req.wh_requested_ie_list.lbs_ie = true;
    if (c->lto) {
        uint32_t ivl = ws_lfn_adjusted_interval(c->bc_interval_ms, proposed_ms, nr->listen_interval_min, nr->listen_interval_max);
        req.wh_requested_ie_list.lto_ie = true;
        req.lto.adjusted_listening_interval = ivl;
        req.lto.offset = ws_lfn_offset(ivl, c->bc_interval_ms);
        tr_info("LPA to %s: LTO offset %"PRIu32" ms, interval %"PRIu32" ms (proposed %"PRIu32")",
                trace_array(dst, 8), (uint32_t)req.lto.offset, ivl, proposed_ms);
    }
    req.wp_requested_nested_ie_list.us_ie = true;
    req.wp_requested_nested_ie_list.bs_ie = true;
    req.wp_requested_nested_ie_list.pan_ie = true;
    req.wp_requested_nested_ie_list.net_name_ie = true;
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
    req.wp_requested_nested_ie_list.lfn_gtk_version_ie = true;
    ws_lfn_lgtk_security_set(cur, &req);
    ws_llc_asynch_request(cur, &req);
}

/* One LPA per sweep: an LPAS sweep reaches us on several channels, and a
 * second LPA (with a second LTO-IE) would move the LFN's schedule twice in
 * our view and once in its own. LPAS from an LFN answered less than
 * WS_LFN_LPA_HOLDOFF_INTERVALS of its proposed intervals ago are ignored. */
#define WS_LFN_LPA_HOLDOFF_INTERVALS 2
#define WS_LFN_LPA_RECENT 8
static struct {
    uint8_t eui64[8];
    uint32_t until_s;
} ws_lfn_lpa_recent[WS_LFN_LPA_RECENT];

static bool ws_lfn_lpa_recently_sent(protocol_interface_info_entry_t *cur, const uint8_t *eui64, uint32_t proposed_ms)
{
    uint32_t now = cur->ws_info->uptime;
    int free_i = -1, oldest = 0;

    for (int i = 0; i < WS_LFN_LPA_RECENT; i++) {
        if (!memcmp(ws_lfn_lpa_recent[i].eui64, eui64, 8) && (int32_t)(ws_lfn_lpa_recent[i].until_s - now) > 0) {
            return true;
        }
        if ((int32_t)(ws_lfn_lpa_recent[i].until_s - now) <= 0) {
            free_i = i;
        } else if (ws_lfn_lpa_recent[i].until_s < ws_lfn_lpa_recent[oldest].until_s) {
            oldest = i;
        }
    }
    if (free_i < 0) {
        free_i = oldest;
    }
    memcpy(ws_lfn_lpa_recent[free_i].eui64, eui64, 8);
    ws_lfn_lpa_recent[free_i].until_s = now + (proposed_ms * WS_LFN_LPA_HOLDOFF_INTERVALS + 999) / 1000 + 1;
    return false;
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

/* Keep an LFN child: it is silent for long, so no FFN timeouts or probes */
static void ws_lfn_child_refresh(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *n)
{
    uint32_t life = (uint32_t)cur->ws_info->lfn_cfg.registration_lifetime_min * 60 * 2;

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
    if (n && ws_lfn_is_child(cur, n, NULL)) {
        ws_lfn_child_refresh(cur, n);
    }
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
    if (!cur->ws_info->lfn_parent) {
        return true;
    }
    if (message_type == WS_FT_LPAS) {
        // Ours if the network name matches; LUTT, LUS, NR and US needed
        const char *why = NULL;
        if (!ws_bootstrap_network_name_matches(ie_ext, cur->ws_info->cfg->gen.network_name)) {
            why = "network name";
        } else if (!ws_wh_nr_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &nr) || nr.node_role != WS_NR_ROLE_LFN) {
            why = "NR-IE";
        } else if (!ws_wh_lus_read(ie_ext->headerIeList, ie_ext->headerIeListLength, &lus)) {
            why = "LUS-IE";
        } else if (!ws_wp_nested_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &us)) {
            why = "US-IE";
        } else if (!ws_bootstrap_validate_channel_plan(&us, cur) || !ws_bootstrap_validate_channel_function(&us, NULL)) {
            why = "channel plan";
        }
        if (why) {
            tr_debug("LPAS from %s ignored (%s)", trace_array(data->SrcAddr, 8), why);
            return true;
        }
        if (ws_lfn_lpa_recently_sent(cur, data->SrcAddr, lus.listen_interval)) {
            tr_debug("LPAS from %s: LPA already sent", trace_array(data->SrcAddr, 8));
            return true;
        }
        if (!ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, true)) {

            tr_warn("LPAS: no room for LFN %s", trace_array(data->SrcAddr, 8));
            return true;
        }
        ws_neighbor_class_neighbor_unicast_schedule_set(cur, ni.ws_neighbor, &us, &cur->ws_info->hopping_schedule, data->SrcAddr);
        if (!ws_llc_lfn_timing_update(cur, ni.ws_neighbor, ie_ext, data->timestamp)) {
            return true;
        }
        ws_lfn_child_refresh(cur, ni.neighbor);
        tr_info("LPAS from LFN %s: interval %"PRIu32" ms", trace_array(data->SrcAddr, 8), (uint32_t)lus.listen_interval);
        ws_lfn_lpa_send(cur, data->SrcAddr, &nr, lus.listen_interval);
        return true;
    }
    // LPCS: from a known LFN, secured
    if (!data->Key.SecurityLevel || !ws_bootstrap_neighbor_info_request(cur, data->SrcAddr, &ni, false)) {
        return true;
    }
    if (ws_wp_nested_us_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &us)) {
        ws_neighbor_class_neighbor_unicast_schedule_set(cur, ni.ws_neighbor, &us, &cur->ws_info->hopping_schedule, data->SrcAddr);
    }
    ws_llc_lfn_timing_update(cur, ni.ws_neighbor, ie_ext, data->timestamp);
    ws_lfn_child_refresh(cur, ni.neighbor);
    mac_neighbor_table_trusted_neighbor(mac_neighbor_info(cur), ni.neighbor, true);
    tr_info("LPCS from LFN %s: LPC", trace_array(data->SrcAddr, 8));
    ws_lfn_lpc_send(cur, data->SrcAddr);
    return true;
}

static void ws_lfn_lpc_round(protocol_interface_info_entry_t *cur)
{
    ns_list_foreach(mac_neighbor_table_entry_t, n, &mac_neighbor_info(cur)->neighbour_list) {
        if (n->trusted_device && ws_lfn_is_child(cur, n, NULL)) {
            ws_lfn_lpc_send(cur, n->mac64);
        }
    }
}

void ws_lfn_parent_seconds_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    ws_lfn_state_t *st = &cur->ws_info->lfn;
    uint32_t period = (cur->ws_info->lfn_cfg.bc_interval_ms * cur->ws_info->lfn_cfg.bc_sync_period + 999) / 1000;

    if (!cur->ws_info->lfn_parent) {
        return;
    }
    if (st->lpc_pending) {
        // An LGTK event: an LPC to every LFN child now
        st->lpc_pending = 0;
        ws_lfn_lpc_round(cur);
    }
    if (st->lpc_timer_s > seconds) {
        st->lpc_timer_s -= seconds;
        return;
    }
    st->lpc_timer_s = period ? period : 1;
    ws_lfn_lpc_round(cur);
}

/* A link-local all-nodes multicast (ff02::1) from the border router: one
 * MAC-unicast copy to each LFN child, sent in its listen window, IPv6
 * destination unchanged (black-box items 41-42). Other groups need the
 * LFN's RFC 9685 subscription, which is not implemented. */
void ws_lfn_parent_multicast_copy(protocol_interface_info_entry_t *cur, buffer_t *buf, bool stable_only, uint_fast16_t max_iphc_size)
{
    const uint8_t *ip_dst = buffer_data_pointer(buf) + 24;

    if (!cur->ws_info->lfn_parent || memcmp(ip_dst, ADDR_LINK_LOCAL_ALL_NODES, 16)) {
        return;
    }
    ns_list_foreach(mac_neighbor_table_entry_t, n, &mac_neighbor_info(cur)->neighbour_list) {
        if (!n->trusted_device || !ws_lfn_is_child(cur, n, NULL)) {
            continue;
        }
        buffer_t *copy = buffer_clone(buf);
        if (!copy) {
            return;
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

bool ws_lfn_parent_neighbor_keep(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *n)
{
    return cur->ws_info->lfn_parent && ws_lfn_is_child(cur, n, NULL);
}

/* ---- LGTK (filled in below by the LGTK step; GTK security until then) */

void ws_lfn_lgtk_security_set(protocol_interface_info_entry_t *cur, asynch_request_t *req)
{
    ws_lfn_security_set(cur, req);
}

void ws_lfn_parent_lgtk_hash_check(protocol_interface_info_entry_t *cur, const struct mcps_data_ie_list *ie_ext)
{
    (void)cur;
    (void)ie_ext;
}

void ws_lfn_parent_lgtk_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    (void)cur;
    (void)seconds;
}

#endif /* HAVE_WS_VERSION_1_1 */
