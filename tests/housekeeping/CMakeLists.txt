cmake_minimum_required(VERSION 3.17)
# Unit test of the housekeeping timers with nothing to do (docs/PORTING.md,
# "Housekeeping with nothing to do"): the stack's ipv6_routing_table.c
# (neighbour and destination caches), mld.c (query timer) and blacklist.c on
# stubs, called every second (periodic) or as the skipping core timer calls
# them. run.sh compares the outputs.
#
#   tests/housekeeping/run.sh [build dir]
project(nanostack-housekeeping-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

foreach(variant periodic skip_prev skip skip_ffn)
    add_executable(test_housekeeping_${variant}
        test_housekeeping.c
        bl_access.c
        stubs_mld.c
        ${ROOT}/nanostack/source/ipv6_stack/ipv6_routing_table.c
        ${ROOT}/nanostack/source/common_protocols/mld.c
        ${ROOT}/mbed-client-libservice/source/libList/ns_list.c
        ${ROOT}/mbed-client-libservice/source/libBits/common_functions.c
    )
    target_include_directories(test_housekeeping_${variant} PRIVATE
        ${ROOT}
        ${ROOT}/include
        ${ROOT}/mbed-client-libservice
        ${ROOT}/nanostack
        ${ROOT}/nanostack/source
        ${ROOT}/nanostack-event-loop
        ${ROOT}/port/posix
    )
    target_compile_definitions(test_housekeeping_${variant} PRIVATE NSCONFIG=ws_router)
    target_compile_options(test_housekeeping_${variant} PRIVATE -std=gnu17 -Wall -Wno-sign-compare -Wno-unused-function -fpermissive)
endforeach()
# periodic: the reference code, called every second
# skip_prev: DI6 without the housekeeping change (the previous wakes)
# skip: with it, as an LFN (the blacklist purge timer is lazy)
# skip_ffn: with it, as an FFN (the purge timer still wakes)
target_compile_definitions(test_housekeeping_skip_prev PRIVATE NS_PROTOCOL_TIMERS_SKIP_IDLE)
target_compile_definitions(test_housekeeping_skip PRIVATE NS_PROTOCOL_TIMERS_SKIP_IDLE NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING PURGE_LAZY=1)
target_compile_definitions(test_housekeeping_skip_ffn PRIVATE NS_PROTOCOL_TIMERS_SKIP_IDLE NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING PURGE_LAZY=0)
