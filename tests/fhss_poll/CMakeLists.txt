cmake_minimum_required(VERSION 3.17)
# Unit test of the idle-poll skipping of the FHSS TX queue (docs/PORTING.md,
# "Idle-poll skipping of the FHSS TX queue"): the stack's FHSS on a fake port
# and a fake MAC, with the periodic poll and with skipping. run.sh compares
# them.
#
#   tests/fhss_poll/run.sh [build dir]
project(nanostack-fhss-poll-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

foreach(variant periodic skip)
    add_executable(test_fhss_poll_${variant}
        test_fhss_poll.c
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
    target_include_directories(test_fhss_poll_${variant} PRIVATE
        ${ROOT}/rcp
        ${ROOT}/rcp/fhss
        ${ROOT}/nanostack
        ${ROOT}/include
        ${ROOT}/nanostack-event-loop
        ${ROOT}/mbed-client-libservice
    )
    target_compile_options(test_fhss_poll_${variant} PRIVATE -std=gnu17 -Wall -Wno-sign-compare -fpermissive)
    target_link_libraries(test_fhss_poll_${variant} PRIVATE m)
endforeach()
# skip: µs callback timers (NS_PORT_TIMER_USEC), as the POSIX, simulation and
# FG25 ports
target_compile_definitions(test_fhss_poll_skip PRIVATE NS_FHSS_POLL_SKIP_IDLE NS_PORT_TIMER_USEC)
