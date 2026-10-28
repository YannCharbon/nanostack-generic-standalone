cmake_minimum_required(VERSION 3.17)
# Unit test of the FAN 1.1 LFN unicast mechanics in the FHSS (docs/LFN_MAC.md):
# the stack's FHSS on a fake port and a fake MAC.
#
#   tests/fhss_lfn/run.sh [build dir]
project(nanostack-fhss-lfn-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_fhss_lfn
    test_fhss_lfn.c
    ${ROOT}/rcp/fhss/channel_functions.c
    ${ROOT}/rcp/fhss/channel_list.c
    ${ROOT}/rcp/fhss/fhss_common.c
    ${ROOT}/rcp/fhss/fhss_configuration_interface.c
    ${ROOT}/rcp/fhss/fhss_statistics.c
    ${ROOT}/rcp/fhss/fhss_ws.c
    ${ROOT}/rcp/fhss/fhss_ws_lfn.c
    ${ROOT}/mbed-client-libservice/source/libBits/common_functions.c
    ${ROOT}/mbed-client-libservice/source/libList/ns_list.c
)
target_include_directories(test_fhss_lfn PRIVATE
    ${ROOT}/rcp
    ${ROOT}/rcp/fhss
    ${ROOT}/nanostack
    ${ROOT}/include
    ${ROOT}/nanostack-event-loop
    ${ROOT}/mbed-client-libservice
)
target_compile_options(test_fhss_lfn PRIVATE -std=gnu17 -Wall -Wno-sign-compare -fpermissive)
target_link_libraries(test_fhss_lfn PRIVATE m)
