/* SPDX-License-Identifier: Apache-2.0
 *
 * Link stubs for mac_mlme.c in test_mac_window.c, apart from the stack's
 * headers: none of them is called on the paths the test drives (an
 * abort() says otherwise). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void mbed_tracef(unsigned char dlevel, const char *grp, const char *fmt, ...) { (void)dlevel; (void)grp; (void)fmt; }
void platform_enter_critical(void) { }
void platform_exit_critical(void) { }
void timer_mac_stop(void *rf_mac_setup) { (void)rf_mac_setup; }

#define STUB(name) void name(void) { fprintf(stderr, "stub called: %s\n", #name); abort(); }
STUB(common_read_16_bit_inverse)
STUB(common_write_16_bit)
STUB(dev_get_phy_datarate)
STUB(eventOS_callback_timer_register)
STUB(eventOS_callback_timer_start)
STUB(eventOS_callback_timer_stop)
STUB(eventOS_callback_timer_unregister)
STUB(eventOS_event_send)
STUB(get_sw_mac_api)
STUB(get_sw_mac_ptr_by_timer)
STUB(mac_cca_thr_deinit)
STUB(mac_cca_thr_init)
STUB(mac_data_edfe_force_stop)
STUB(mac_dst_panid_present)
STUB(mac_filter_add_long)
STUB(mac_filter_clear)
STUB(mac_filter_start)
STUB(mac_filter_stop)
STUB(mac_generic_event_trig)
STUB(mac_get_cca_threshold_table)
STUB(mac_header_address_length)
STUB(mac_header_security_aux_header_length)
STUB(mac_header_security_parameter_set)
STUB(mac_mcps_buffer_queue_free)
STUB(mac_mcps_sap_tasklet_init)
STUB(mac_mcps_trig_buffer_from_queue)
STUB(mac_pd_sap_rf_low_level_function_set)
STUB(mac_sec_key_description_get_by_attribute)
STUB(mac_sec_mib_deinit)
STUB(mac_sec_mib_device_description_get_attribute_index)
STUB(mac_sec_mib_device_description_set)
STUB(mac_sec_mib_init)
STUB(mac_sec_mib_key_description_set)
STUB(mac_sec_mib_key_outgoing_frame_counter_set)
STUB(mac_security_mic_length_get)
STUB(mac_src_panid_present)
STUB(mcps_sap_pd_req_queue_write)
STUB(mcps_sap_prebuild_frame_buffer_free)
STUB(mcps_sap_prebuild_frame_buffer_get)
STUB(rand_get_8bit)
STUB(sw_mac_stats_update)
STUB(timer_mac_interrupt)
