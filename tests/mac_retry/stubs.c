/* SPDX-License-Identifier: Apache-2.0
 *
 * Link stubs for mac_pd_sap.c in test_mac_retry.c, apart from the stack's
 * headers: none of them is called on the path the test drives,
 * mac_csma_backoff_get() (an abort() says otherwise). */
#include <stdio.h>
#include <stdlib.h>

void mbed_tracef(unsigned char dlevel, const char *grp, const char *fmt, ...) { (void)dlevel; (void)grp; (void)fmt; }

#define STUB(name) void name(void) { fprintf(stderr, "stub called: %s\n", #name); abort(); }
STUB(common_read_32_bit)
STUB(common_write_16_bit)
STUB(get_sw_mac_api)
STUB(mac_address_length)
STUB(mac_cca_threshold_event_send)
STUB(mac_cca_thr_get_dbm)
STUB(mac_fhss_frame_type)
STUB(mac_filter_modify_link_quality)
STUB(mac_header_address_length)
STUB(mac_header_get_dst_address)
STUB(mac_header_get_dst_panid)
STUB(mac_header_get_src_address)
STUB(mac_header_information_elements_parse)
STUB(mac_header_parse_fcf_dsn)
STUB(mac_is_ack_request_set)
STUB(mac_mcps_sap_get_phy_timestamp)
STUB(mac_mlme_rf_channel_change)
STUB(mac_parse_mode_switch_phr)
STUB(mac_security_mic_length_get)
STUB(mac_update_mode_switch_state)
STUB(mcps_edfe_data_request)
STUB(mcps_generic_ack_build)
STUB(mcps_generic_ack_data_request_init)
STUB(mcps_generic_edfe_frame_init)
STUB(mcps_pd_data_rebuild)
STUB(mcps_pending_packet_counter_update_check)
STUB(mcps_sap_pd_ack)
STUB(mcps_sap_pd_confirm)
STUB(mcps_sap_pd_ind)
STUB(mcps_sap_pre_parsed_ack_buffer_get)
STUB(mcps_sap_pre_parsed_frame_buffer_free)
STUB(mcps_sap_pre_parsed_frame_buffer_get)
STUB(mlme_scan_analyze_next_channel)
STUB(sw_mac_stats_update)
STUB(timer_mac_start)
STUB(timer_mac_stop)
