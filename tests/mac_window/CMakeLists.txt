cmake_minimum_required(VERSION 3.17)
# Unit test of the LFN's receive windows in the MAC (mac_mlme.c,
# mac_mlme_rx_window()) and of its rule for turning the radio off after a
# data confirm (mac_lfn_async.c, mac_lfn_radio_stays_on()), on a fake PHY.
#
#   tests/mac_window/run.sh [build dir]
project(nanostack-mac-window C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(test_mac_window
    test_mac_window.c
    stubs.c
    ${ROOT}/rcp/mac/ieee802154/mac_mlme.c
    ${ROOT}/rcp/mac/ieee802154/mac_lfn_async.c
)
target_include_directories(test_mac_window PRIVATE
    ${ROOT}
    ${ROOT}/include
    ${ROOT}/mbed-client-libservice
    ${ROOT}/nanostack
    ${ROOT}/nanostack-event-loop
    ${ROOT}/nanostack/source
    ${ROOT}/port/posix
    ${ROOT}/rcp
    ${ROOT}/rcp/mac
    ${ROOT}/rcp/mac/ieee802154
)
target_compile_definitions(test_mac_window PRIVATE NSCONFIG=ws_router)
target_compile_options(test_mac_window PRIVATE -std=gnu17 -Wall -Wno-sign-compare -fpermissive)
