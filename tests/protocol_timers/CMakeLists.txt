cmake_minimum_required(VERSION 3.17)
# Unit test of the idle-tick skipping of the protocol timers (docs/PORTING.md,
# "Idle-tick skipping of the protocol timers"): the stack's protocol timer
# and core timer (protocol_timer.c, protocol_core.c) with fake modules on a
# fake port, periodic against skipping. run.sh compares them.
#
#   tests/protocol_timers/run.sh [build dir]
project(nanostack-protocol-timers-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

foreach(variant periodic skip skip_slots)
    add_executable(test_protocol_timers_${variant}
        test_protocol_timers.c
        ${ROOT}/nanostack/source/nwk_interface/protocol_timer.c
        ${ROOT}/nanostack/source/nwk_interface/protocol_core.c
        ${ROOT}/nanostack-event-loop/source/event.c
        ${ROOT}/nanostack-event-loop/source/system_timer.c
        ${ROOT}/mbed-client-libservice/source/libList/ns_list.c
    )
    target_include_directories(test_protocol_timers_${variant} PRIVATE
        ${ROOT}
        ${ROOT}/include
        ${ROOT}/nanostack
        ${ROOT}/nanostack/source
        ${ROOT}/nanostack-event-loop
        ${ROOT}/nanostack-event-loop/source
        ${ROOT}/mbed-client-libservice
    )
    # The event loop's own ticks are skipped in every variant: the test uses
    # no event timer
    target_compile_definitions(test_protocol_timers_${variant} PRIVATE
        NSCONFIG=ws_router NS_EVENTLOOP_SKIP_IDLE_TICKS)
    target_compile_options(test_protocol_timers_${variant} PRIVATE -std=gnu17 -Wall -Wno-sign-compare -Wno-unused-function -fpermissive)
endforeach()
# skip: a port with µs timers (NS_PORT_TIMER_USEC), as the POSIX and
# simulation ports; skip_slots: 50 µs slots only, as the FG25 port
target_compile_definitions(test_protocol_timers_skip PRIVATE NS_PROTOCOL_TIMERS_SKIP_IDLE NS_PORT_TIMER_USEC)
target_compile_definitions(test_protocol_timers_skip_slots PRIVATE NS_PROTOCOL_TIMERS_SKIP_IDLE)
