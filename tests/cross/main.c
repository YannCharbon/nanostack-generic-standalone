/*
 * Cross-build check: the in-process init sequence of GENERICITY_PLAN.md §3.3,
 * for a border router (ROLE_BR) or a router (ROLE_RN), on a stub radio. It is
 * linked, never run.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "mbed-client-libservice/ns_trace.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack-event-loop/eventOS_scheduler.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/sw_mac.h"
#include "nanostack/mac/fhss_config.h"
#include "nanostack/mac/platform/arm_hal_phy.h"
#include "nanostack/net_interface.h"
#include "nanostack/ws_management_api.h"
#include "nanostack/ethernet_mac_api.h"
#include "nanostack/ws_bbr_api.h"
#include "ns_port/ns_port.h"

static uint8_t eui64[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static int8_t ws_if = -1, bh_if = -1;

static int8_t radio_state(phy_interface_state_e state, uint8_t channel) { return 0; }
static int8_t radio_tx(uint8_t *buf, uint16_t len, uint8_t handle, data_protocol_e proto) { return 0; }
static int8_t radio_extension(phy_extension_type_e type, uint8_t *data) { return 0; }
static int8_t radio_address(phy_address_type_e type, uint8_t *addr) { return 0; }

static phy_device_channel_page_s pages[] = {
    { CHANNEL_PAGE_2, NULL },
    { CHANNEL_PAGE_0, NULL },
};

static phy_device_driver_s radio = {
    .link_type = PHY_LINK_15_4_SUBGHZ_TYPE,
    .data_request_layer = PHY_LAYER_PAYLOAD_DATA_FLOW,
    .PHY_MAC = eui64,
    .phy_MTU = 2043,
    .driver_description = (char *)"STUB",
    .phy_channel_pages = pages,
    .state_control = radio_state,
    .tx = radio_tx,
    .extension = radio_extension,
    .address_write = radio_address,
};

#ifdef ROLE_BR
static uint8_t bh_mac[8] = { 20, 21, 22, 23, 24, 25, 26, 27 };
static phy_device_driver_s backhaul = {
    .link_type = PHY_LINK_TUN,
    .data_request_layer = IPV6_DATAGRAMS_DATA_FLOW,
    .PHY_MAC = bh_mac,
    .driver_description = (char *)"NULL BH",
    .tx = radio_tx,
};
#endif

static void tasklet(arm_event_t *ev)
{
    if (ev->event_type != ARM_LIB_TASKLET_INIT_EVENT)
        return;
#ifdef ROLE_BR
    static const uint8_t prefix[16] = { 0xfd, 0, 0x61, 0x72, 0x6d };
    arm_nwk_interface_configure_6lowpan_bootstrap_set(ws_if, NET_6LOWPAN_BORDER_ROUTER, NET_6LOWPAN_WS);
    arm_nwk_interface_configure_ipv6_bootstrap_set(bh_if, NET_IPV6_BOOTSTRAP_STATIC, prefix);
#else
    arm_nwk_interface_configure_6lowpan_bootstrap_set(ws_if, NET_6LOWPAN_ROUTER, NET_6LOWPAN_WS);
#endif
    ws_management_node_init(ws_if, REG_DOMAIN_EU, (char *)"cross", (fhss_timer_t *)&ns_port_fhss_timer);
    ws_management_network_size_set(ws_if, NETWORK_SIZE_SMALL);
#ifdef ROLE_BR
    arm_nwk_interface_up(bh_if);
    arm_nwk_interface_up(ws_if);
    ws_bbr_start(ws_if, bh_if);
#else
    arm_nwk_interface_up(ws_if);
#endif
}

int main(void)
{
    mac_description_storage_size_t sizes = { 32, 4, 1, 3 };
    mac_api_t *mac;
    int8_t rf_id;

    platform_critical_init();
    mbed_trace_init();
    mbed_trace_print_function_set(ns_port_trace_write);
    eventOS_scheduler_init();
    rf_id = arm_net_phy_register(&radio);
    arm_net_phy_mac64_set(eui64, rf_id);
    mac = ns_sw_mac_create(rf_id, &sizes);
    net_init_core();
    ws_if = arm_nwk_interface_lowpan_init(mac, (char *)"ws0");
#ifdef ROLE_BR
    bh_if = arm_nwk_interface_ethernet_init(ethernet_mac_create(arm_net_phy_register(&backhaul)), (char *)"bh0");
#endif
    eventOS_event_handler_create(tasklet, ARM_LIB_TASKLET_INIT_EVENT);
    ns_port_run();
}
