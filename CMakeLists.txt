cmake_minimum_required(VERSION 3.17)
project(nanostack-generic-standalone C)
# Nanostack (Silicon Labs wisun-br-linux v1.2 flavour) extracted from
# wisun-mbed-simulator. This file only declares libraries: the executables
# that use them (border router, router, RCP) live in the consumer project.
#
# Targets:
#   nanostack_port_api          include/: the porting API (ns_port/*.h) and
#                               the stack's rand_* helpers (ns_rand.h)
#   nanostack_libservice        mbed-client-libservice (ns_list, ns_trace, ...)
#   nanostack_eventloop         nanostack-event-loop
#   nanostack_port_posix        POSIX platform layer in port/posix (timers,
#                               FHSS timer, scheduler, critical sections,
#                               random, logging). Only built when it is the
#                               selected port (NANOSTACK_PORT)
# Functions (a Nanostack build depends on compile-time configuration):
#   nanostack_add_library(<name> CONFIG <nsconfig> [BORDER_ROUTER]
#                         [DEFINITIONS <def>...])
#                               6LoWPAN/IPv6/RPL/MPL/Wi-SUN upper layers
#   nanostack_add_rcp_library(<name> [INPROCESS] [DEFINITIONS <def>...])
#                               802.15.4 MAC, FHSS and CCM (RCP side).
#                               INPROCESS: the MAC is linked into the same
#                               image as the upper layers, so rcp/'s copy of
#                               rf_driver_storage.c is left out and
#                               nanostack/'s is used (D15)
#
# Options (GENERICITY_PLAN.md D9, D15, D18):
#   NANOSTACK_PORT              target implementing the porting API
#                               (include/ns_port/). Default
#                               nanostack_port_posix. Another port is a target
#                               of the consumer, which links nanostack_port_api;
#                               then Threads, backtrace and libdl are not used.
#   NANOSTACK_FILE_NO_STDIO     compile out the stdio fallback of
#                               ns_file_system.c, for targets without files
#   NANOSTACK_TLS_STRONG_PORT_ENTROPY
#                               register the port's random source as a STRONG
#                               TLS entropy source: only if it is a real TRNG
# mbedTLS: if the consumer already defines MbedTLS::mbedtls,
# MbedTLS::mbedcrypto and MbedTLS::mbedx509, they are used as they are;
# otherwise find_package(MbedTLS) looks for v3.0.0.

set(NANOSTACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(NANOSTACK_PORT nanostack_port_posix CACHE STRING "Target implementing the Nanostack porting API")
option(NANOSTACK_FILE_NO_STDIO "Compile out the stdio fallback of ns_file_system.c" OFF)
option(NANOSTACK_TLS_STRONG_PORT_ENTROPY "The port's random source is a TRNG: strong TLS entropy source" OFF)

if(NANOSTACK_PORT STREQUAL "nanostack_port_posix")
    include(CheckIncludeFile)
    include(CheckSymbolExists)
    check_include_file(backtrace.h BACKTRACE_FOUND)
    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS})
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(dladdr dlfcn.h LIBDL_FOUND)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_DEFINITIONS)

    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()
if(NOT TARGET MbedTLS::mbedtls)
    find_package(MbedTLS REQUIRED)
    mark_as_advanced(MbedTLS_DIR)
endif()

# Headers shared by the stack and every port: the porting API a port
# implements (GENERICITY_PLAN.md §3) and the stack's rand_* helpers.
# ns_port/timer.h and sched.h include the event loop's own declarations.
add_library(nanostack_port_api INTERFACE)
target_include_directories(nanostack_port_api INTERFACE
    ${NANOSTACK_ROOT}/include
    ${NANOSTACK_ROOT}/nanostack-event-loop/
)

# The repository root is kept on the include path, as it was in
# wisun-mbed-simulator, so that "nanostack/...", "mbed-client-libservice/..."
# and "nanostack-event-loop/..." style includes resolve unchanged.
add_library(nanostack_libservice STATIC
    source/ns_rand.c
    mbed-client-libservice/source/libBits/common_functions.c
    # Excluded mbed-client-libservice/source/libip4string
    mbed-client-libservice/source/libip6string/ip6tos.c
    mbed-client-libservice/source/libip6string/stoip6.c
    mbed-client-libservice/source/libList/ns_list.c
    mbed-client-libservice/source/libtrace/ns_trace.c
    # Excluded mbed-client-libservice/source/nvmHelper
)
target_include_directories(nanostack_libservice PUBLIC
    ${NANOSTACK_ROOT}
    ${NANOSTACK_ROOT}/mbed-client-libservice/
)
# ns_rand.c gets its random bytes from the port.
target_link_libraries(nanostack_libservice PUBLIC nanostack_port_api ${NANOSTACK_PORT})

add_library(nanostack_eventloop STATIC
    nanostack-event-loop/source/event.c
    nanostack-event-loop/source/system_timer.c
)
target_include_directories(nanostack_eventloop PUBLIC
    ${NANOSTACK_ROOT}/nanostack-event-loop/
)
target_link_libraries(nanostack_eventloop PUBLIC nanostack_libservice ${NANOSTACK_PORT})

if(NANOSTACK_PORT STREQUAL "nanostack_port_posix")
    # Includes stay "common/xxx.h", as in the original tree.
    add_library(nanostack_port_posix STATIC
        port/posix/common/fhss_timer.c
        port/posix/common/hal_interrupt.c
        port/posix/common/os_timer.c
        port/posix/common/os_scheduler.c
        port/posix/common/log.c
        port/posix/common/random.c
        port/posix/common/slist.c
        port/posix/common/time.c
        port/posix/common/trace.c
    )
    target_include_directories(nanostack_port_posix PUBLIC
        ${NANOSTACK_ROOT}/port/posix/
    )
    # fhss_timer.c uses the stack's nanostack/mac/fhss_config.h.
    target_include_directories(nanostack_port_posix PRIVATE ${NANOSTACK_ROOT}/nanostack/)
    target_link_libraries(nanostack_port_posix PUBLIC Threads::Threads nanostack_eventloop)
    if(BACKTRACE_FOUND)
        target_compile_definitions(nanostack_port_posix PUBLIC HAVE_BACKTRACE)
        target_sources(nanostack_port_posix PRIVATE port/posix/common/backtrace_show.c)
        target_link_libraries(nanostack_port_posix PUBLIC backtrace)
    endif()
    if(LIBDL_FOUND)
        target_compile_definitions(nanostack_port_posix PRIVATE HAVE_LIBDL)
        target_link_libraries(nanostack_port_posix PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

set(NANOSTACK_SOURCES
    nanostack/source/6lowpan/adaptation_interface.c
    nanostack/source/6lowpan/bootstraps/network_lib.c
    nanostack/source/6lowpan/bootstraps/protocol_6lowpan.c
    nanostack/source/6lowpan/bootstraps/protocol_6lowpan_bootstrap.c
    nanostack/source/6lowpan/bootstraps/protocol_6lowpan_interface.c
    nanostack/source/6lowpan/fragmentation/cipv6_fragmenter.c
    nanostack/source/6lowpan/iphc_decode/6lowpan_iphc.c
    nanostack/source/6lowpan/iphc_decode/iphc_compress.c
    nanostack/source/6lowpan/iphc_decode/iphc_decompress.c
    nanostack/source/6lowpan/iphc_decode/lowpan_context.c
    nanostack/source/6lowpan/mac/mac_helper.c
    nanostack/source/6lowpan/mac/mac_ie_lib.c
    nanostack/source/6lowpan/mac/mac_response_handler.c
    nanostack/source/6lowpan/nd/nd_router_object.c
    nanostack/source/6lowpan/ws/ws_bootstrap.c
    nanostack/source/6lowpan/ws/ws_bootstrap_ffn.c
    nanostack/source/6lowpan/ws/ws_bootstrap_lfn.c
    nanostack/source/6lowpan/ws/ws_cfg_settings.c
    nanostack/source/6lowpan/ws/ws_common.c
    nanostack/source/6lowpan/ws/ws_eapol_auth_relay.c
    nanostack/source/6lowpan/ws/ws_eapol_pdu.c
    nanostack/source/6lowpan/ws/ws_eapol_relay.c
    nanostack/source/6lowpan/ws/ws_eapol_relay_lib.c
    nanostack/source/6lowpan/ws/ws_ie_lib.c
    nanostack/source/6lowpan/ws/ws_llc_data_service.c
    nanostack/source/6lowpan/ws/ws_management_api.c
    nanostack/source/6lowpan/ws/ws_mpx_header.c
    nanostack/source/6lowpan/ws/ws_neighbor_class.c
    nanostack/source/6lowpan/ws/ws_pae_auth.c
    nanostack/source/6lowpan/ws/ws_pae_controller.c
    nanostack/source/6lowpan/ws/ws_pae_key_storage.c
    nanostack/source/6lowpan/ws/ws_pae_lib.c
    nanostack/source/6lowpan/ws/ws_pae_nvm_store.c
    nanostack/source/6lowpan/ws/ws_pae_nvm_data.c
    nanostack/source/6lowpan/ws/ws_pae_supp.c
    nanostack/source/6lowpan/ws/ws_pae_time.c
    nanostack/source/6lowpan/ws/ws_pae_timers.c
    nanostack/source/6lowpan/ws/ws_phy.c
    nanostack/source/6lowpan/ws/ws_regulation_arib.c
    nanostack/source/6lowpan/ws/ws_regulation_none.c
    nanostack/source/6lowpan/ws/ws_regulation.c
    nanostack/source/6lowpan/ws/ws_stats.c
    nanostack/source/6lowpan/ws/ws_test_api.c
    nanostack/source/common_protocols/icmpv6.c
    nanostack/source/common_protocols/icmpv6_prefix.c
    nanostack/source/common_protocols/icmpv6_radv.c
    nanostack/source/common_protocols/ipv6.c
    nanostack/source/common_protocols/ipv6_flow.c
    nanostack/source/common_protocols/ipv6_fragmentation.c
    nanostack/source/common_protocols/ipv6_resolution.c
    nanostack/source/common_protocols/mld.c
    nanostack/source/common_protocols/udp.c
    nanostack/source/common_protocols/tcp.c
    nanostack/source/core/buffer_dyn.c
    nanostack/source/core/ns_address_internal.c
    nanostack/source/core/ns_socket.c
    nanostack/source/core/sockbuf.c
    nanostack/source/dhcpv6_client/dhcpv6_client_service.c
    nanostack/source/ipv6_stack/ipv6_routing_table.c
    nanostack/source/ipv6_stack/protocol_ipv6.c
    nanostack/source/libdhcpv6/dhcp_service_api.c
    nanostack/source/libdhcpv6/libdhcpv6.c
    nanostack/source/libdhcpv6/libdhcpv6_vendordata.c
    nanostack/source/net_lib/net_short_address_extension.c
    nanostack/source/net_lib/net_dns.c
    nanostack/source/net_lib/multicast_api.c
    nanostack/source/net_lib/net_6lowpan_parameter_api.c
    nanostack/source/net_lib/net_rpl.c
    nanostack/source/net_lib/ns_net.c
    nanostack/source/net_lib/socket_api.c
    nanostack/source/net_lib/net_ipv6.c
    nanostack/source/mac/rf_driver_storage.c
    nanostack/source/mac/ethernet/ethernet_mac_api.c
    nanostack/source/mpl/mpl.c
    nanostack/source/nwk_interface/protocol_core.c
    nanostack/source/nwk_interface/protocol_stats.c
    nanostack/source/nwk_interface/protocol_timer.c
    nanostack/source/rpl/rpl_control.c
    nanostack/source/rpl/rpl_mrhof.c
    nanostack/source/rpl/rpl_downward.c
    nanostack/source/rpl/rpl_data.c
    nanostack/source/rpl/rpl_objective.c
    nanostack/source/rpl/rpl_of0.c
    nanostack/source/rpl/rpl_policy.c
    nanostack/source/rpl/rpl_upward.c
    nanostack/source/security/eapol/eapol_helper.c
    nanostack/source/security/eapol/kde_helper.c
    nanostack/source/security/kmp/kmp_addr.c
    nanostack/source/security/kmp/kmp_api.c
    nanostack/source/security/kmp/kmp_eapol_pdu_if.c
    nanostack/source/security/kmp/kmp_socket_if.c
    nanostack/source/security/pana/pana_eap_header.c
    nanostack/source/security/protocols/eap_tls_sec_prot/auth_eap_tls_sec_prot.c
    nanostack/source/security/protocols/eap_tls_sec_prot/eap_tls_sec_prot_lib.c
    nanostack/source/security/protocols/eap_tls_sec_prot/radius_eap_tls_sec_prot.c
    nanostack/source/security/protocols/eap_tls_sec_prot/supp_eap_tls_sec_prot.c
    nanostack/source/security/protocols/fwh_sec_prot/auth_fwh_sec_prot.c
    nanostack/source/security/protocols/fwh_sec_prot/supp_fwh_sec_prot.c
    nanostack/source/security/protocols/gkh_sec_prot/auth_gkh_sec_prot.c
    nanostack/source/security/protocols/gkh_sec_prot/supp_gkh_sec_prot.c
    nanostack/source/security/protocols/key_sec_prot/key_sec_prot.c
    nanostack/source/security/protocols/msg_sec_prot/msg_sec_prot.c
    nanostack/source/security/protocols/radius_sec_prot/avp_helper.c
    nanostack/source/security/protocols/radius_sec_prot/radius_client_sec_prot.c
    nanostack/source/security/protocols/sec_prot_certs.c
    nanostack/source/security/protocols/sec_prot_keys.c
    nanostack/source/security/protocols/sec_prot_lib.c
    nanostack/source/security/protocols/tls_sec_prot/tls_sec_prot.c
    nanostack/source/security/protocols/tls_sec_prot/tls_sec_prot_lib.c
    nanostack/source/service_libs/blacklist/blacklist.c
    nanostack/source/service_libs/etx/etx.c
    nanostack/source/service_libs/fnv_hash/fnv_hash.c
    nanostack/source/service_libs/hmac/hmac_md.c
    nanostack/source/service_libs/ieee_802_11/ieee_802_11.c
    nanostack/source/service_libs/mac_neighbor_table/mac_neighbor_table.c
    nanostack/source/service_libs/nd_proxy/nd_proxy.c
    nanostack/source/service_libs/nist_aes_kw/nist_aes_kw.c
    nanostack/source/service_libs/random_early_detection/random_early_detection.c
    nanostack/source/service_libs/sha256/ns_sha256.c
    nanostack/source/service_libs/sha256/shalib.c
    nanostack/source/service_libs/trickle/trickle.c
    nanostack/source/service_libs/utils/isqrt.c
    nanostack/source/service_libs/utils/ns_file_system.c
    nanostack/source/service_libs/utils/ns_time.c
)
# Only built into border router variants
set(NANOSTACK_BR_SOURCES
    nanostack/source/6lowpan/ws/ws_bbr_api.c
    nanostack/source/6lowpan/ws/ws_bootstrap_6lbr.c
    nanostack/source/dhcpv6_server/dhcpv6_server_service.c
    nanostack/source/libdhcpv6/libdhcpv6_server.c
)
list(TRANSFORM NANOSTACK_SOURCES PREPEND ${NANOSTACK_ROOT}/)
list(TRANSFORM NANOSTACK_BR_SOURCES PREPEND ${NANOSTACK_ROOT}/)
# The functions below are called from the consumer's directory scope, so the
# lists they use are published in the cache.
set(NANOSTACK_ROOT ${NANOSTACK_ROOT} CACHE INTERNAL "")
set(NANOSTACK_SOURCES ${NANOSTACK_SOURCES} CACHE INTERNAL "")
set(NANOSTACK_BR_SOURCES ${NANOSTACK_BR_SOURCES} CACHE INTERNAL "")

function(nanostack_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "BORDER_ROUTER" "CONFIG" "DEFINITIONS")
    if(NOT ARG_CONFIG)
        message(FATAL_ERROR "nanostack_add_library(${name}): CONFIG is required (e.g. ws_router)")
    endif()
    add_library(${name} STATIC ${NANOSTACK_SOURCES})
    if(ARG_BORDER_ROUTER)
        target_sources(${name} PRIVATE ${NANOSTACK_BR_SOURCES})
    endif()
    target_include_directories(${name} PUBLIC
        ${NANOSTACK_ROOT}/nanostack/source/
        ${NANOSTACK_ROOT}/nanostack/
    )
    # NSCONFIG selects nanostack/source/configs/cfg_<nsconfig>.h; it is public
    # because the stack's internal headers are used by the applications.
    target_compile_definitions(${name} PUBLIC NSCONFIG=${ARG_CONFIG})
    if(ARG_DEFINITIONS)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
    endif()
    if(NANOSTACK_FILE_NO_STDIO)
        target_compile_definitions(${name} PRIVATE NS_FILE_NO_STDIO)
    endif()
    if(NANOSTACK_TLS_STRONG_PORT_ENTROPY)
        target_compile_definitions(${name} PRIVATE NS_TLS_STRONG_PORT_ENTROPY)
    endif()
    target_link_libraries(${name} PUBLIC
        ${NANOSTACK_PORT} nanostack_eventloop nanostack_libservice
        MbedTLS::mbedtls MbedTLS::mbedcrypto MbedTLS::mbedx509)
endfunction()

set(NANOSTACK_RCP_SOURCES
    rcp/mac/rf_driver_storage.c
    rcp/mac/ieee802154/mac_cca_threshold.c
    rcp/mac/ieee802154/mac_fhss_callbacks.c
    rcp/mac/ieee802154/mac_filter.c
    rcp/mac/ieee802154/mac_header_helper_functions.c
    rcp/mac/ieee802154/mac_indirect_data.c
    rcp/mac/ieee802154/mac_mcps_sap.c
    rcp/mac/ieee802154/mac_mlme.c
    rcp/mac/ieee802154/mac_mode_switch.c
    rcp/mac/ieee802154/mac_pd_sap.c
    rcp/mac/ieee802154/mac_security_mib.c
    rcp/mac/ieee802154/mac_timer.c
    rcp/mac/ieee802154/sw_mac.c
    rcp/ccm/ccm_security.c
    rcp/ccm/mbedOS/aes_mbedtls_adapter.c
    rcp/fhss/channel_functions.c
    rcp/fhss/channel_list.c
    rcp/fhss/fhss_common.c
    rcp/fhss/fhss_configuration_interface.c
    rcp/fhss/fhss_statistics.c
    rcp/fhss/fhss_ws.c
    # Excluded rcp/fhss/fhss_test_api.c
)
list(TRANSFORM NANOSTACK_RCP_SOURCES PREPEND ${NANOSTACK_ROOT}/)
set(NANOSTACK_RCP_SOURCES ${NANOSTACK_RCP_SOURCES} CACHE INTERNAL "")

# The RCP side has its own copy of mac/rf_driver_storage.h, so rcp/ comes
# before nanostack/ and nanostack/source/ is not on its include path.
function(nanostack_add_rcp_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "INPROCESS" "" "DEFINITIONS")
    set(srcs ${NANOSTACK_RCP_SOURCES})
    if(ARG_INPROCESS)
        list(REMOVE_ITEM srcs ${NANOSTACK_ROOT}/rcp/mac/rf_driver_storage.c)
    endif()
    add_library(${name} STATIC ${srcs})
    target_include_directories(${name} PUBLIC
        ${NANOSTACK_ROOT}/rcp/
        ${NANOSTACK_ROOT}/nanostack/
    )
    if(ARG_DEFINITIONS)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
    endif()
    target_link_libraries(${name} PUBLIC
        ${NANOSTACK_PORT} nanostack_eventloop nanostack_libservice
        MbedTLS::mbedtls m)
endfunction()

# Standalone build: compile the default variants so the stack can be built
# and checked on its own.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    nanostack_add_library(nanostack_ws_border_router CONFIG ws_border_router BORDER_ROUTER)
    nanostack_add_library(nanostack_ws_router CONFIG ws_router)
    nanostack_add_rcp_library(nanostack_rcp)
endif()
