/*
 * Copyright (c) 2026, the mesh-emulator-rework authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-SUN FAN 1.1 LFN parent (border router) and LGTK handling
 * (GS8 stage 3; docs/LFN_STACK.md).
 */
#ifndef WS_LFN_PARENT_H_
#define WS_LFN_PARENT_H_

#include <stdbool.h>
#include <stdint.h>

struct protocol_interface_info_entry;
struct mcps_data_ind_s;
struct mcps_data_ie_list;
struct mac_neighbor_table_entry;
struct asynch_request_s;
struct buffer;

#ifdef HAVE_WS_VERSION_1_1
/* Parent (border router): LPAS and LPCS. Returns true if the frame was an
 * LFN management frame (handled or dropped here). */
bool ws_lfn_parent_asynch_ind(struct protocol_interface_info_entry *cur, const struct mcps_data_ind_s *data, const struct mcps_data_ie_list *ie_ext, uint8_t message_type);
void ws_lfn_parent_seconds_timer(struct protocol_interface_info_entry *cur, uint32_t seconds);
/* A frame from a neighbour: an LFN child's entry is kept */
void ws_lfn_parent_frame_rx(struct protocol_interface_info_entry *cur, const uint8_t *src64);
/* A multicast to ff02::1: a MAC-unicast copy to each LFN child */
void ws_lfn_parent_multicast_copy(struct protocol_interface_info_entry *cur, struct buffer *buf, bool stable_only, uint_fast16_t max_iphc_size);
/* An LFN child: no FFN probing or NUD */
bool ws_lfn_parent_neighbor_keep(struct protocol_interface_info_entry *cur, struct mac_neighbor_table_entry *n);
/* Security of frames to and from LFNs (the active LGTK when there is one) */
void ws_lfn_lgtk_security_set(struct protocol_interface_info_entry *cur, struct asynch_request_s *req);
/* LFN: LGTK hash of an LPC against ours; key request after the mismatch time */
void ws_lfn_parent_lgtk_hash_check(struct protocol_interface_info_entry *cur, const struct mcps_data_ie_list *ie_ext);
void ws_lfn_parent_lgtk_timer(struct protocol_interface_info_entry *cur, uint32_t seconds);
/* Border router: LGTKs changed (ws_pae_controller.c) */
struct sec_prot_gtk_keys_s;
void ws_lfn_parent_lgtk_update(struct protocol_interface_info_entry *cur, struct sec_prot_gtk_keys_s *lgtks, int8_t active_index, const char *event);
/* Border router: the LGTK key index for a frame to an LFN child, 0 otherwise */
uint8_t ws_lfn_parent_tx_key_index(struct protocol_interface_info_entry *cur, const uint8_t *dst64);
/* Parent: the destination is an LFN child (frames to it carry an LBT-IE) */
bool ws_lfn_parent_dst_is_lfn(struct protocol_interface_info_entry *cur, const uint8_t *dst64);
#else
#define ws_lfn_parent_dst_is_lfn(cur, dst64) (false)
#define ws_lfn_parent_lgtk_update(cur, lgtks, active_index, event) ((void)0)
#define ws_lfn_parent_tx_key_index(cur, dst64) (0)
#define ws_lfn_parent_asynch_ind(cur, data, ie_ext, type) (false)
#define ws_lfn_parent_seconds_timer(cur, seconds) ((void)0)
#define ws_lfn_parent_frame_rx(cur, src64) ((void)0)
#define ws_lfn_parent_neighbor_keep(cur, n) (false)
#define ws_lfn_parent_multicast_copy(cur, buf, stable_only, max) ((void)0)
#define ws_lfn_parent_lgtk_hash_check(cur, ie_ext) ((void)0)
#define ws_lfn_parent_lgtk_timer(cur, seconds) ((void)0)
#endif

#endif /* WS_LFN_PARENT_H_ */
