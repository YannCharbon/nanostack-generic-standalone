cmake_minimum_required(VERSION 3.17)
# Unit tests of LFN code outside the FHSS (docs/LFN_STACK.md §11): the MAC's
# hold-back of an asynchronous sweep (mac_lfn_async.c), the duplicate
# detection (ws_neighbor_class.c) and the TTL of frames to LFN children in
# the adaptation layer (adaptation_interface.c), on stubs.
#
#   tests/lfn_units/run.sh [build dir]
project(nanostack-lfn-units C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
option(OLD_STACK "Built against a stack before the fixes (no mac_lfn_async.c, the former duplicate check)" OFF)

set(SRC
    test_lfn_units.c
    ${ROOT}/nanostack/source/6lowpan/ws/ws_neighbor_class.c
    ${ROOT}/nanostack/source/6lowpan/adaptation_interface.c
    ${ROOT}/mbed-client-libservice/source/libList/ns_list.c
    ${ROOT}/mbed-client-libservice/source/libBits/common_functions.c
)
if(NOT OLD_STACK)
    list(APPEND SRC ${ROOT}/rcp/mac/ieee802154/mac_lfn_async.c)
endif()
add_executable(test_lfn_units ${SRC})
target_include_directories(test_lfn_units PRIVATE
    ${ROOT}
    ${ROOT}/include
    ${ROOT}/mbed-client-libservice
    ${ROOT}/nanostack
    ${ROOT}/nanostack-event-loop
    ${ROOT}/nanostack/source
    ${ROOT}/port/posix
    ${ROOT}/rcp
    ${ROOT}/rcp/mac/ieee802154
)
target_compile_definitions(test_lfn_units PRIVATE NSCONFIG=ws_border_router $<$<BOOL:${OLD_STACK}>:OLD_STACK>)
target_compile_options(test_lfn_units PRIVATE -std=gnu17 -Wall -Wno-sign-compare -fpermissive)
