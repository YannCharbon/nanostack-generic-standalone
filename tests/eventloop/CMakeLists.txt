cmake_minimum_required(VERSION 3.17)
# Unit test of the event loop's timers (docs/PORTING.md, "Idle-tick
# skipping"): the same schedule with the periodic tick and with idle-tick skipping, on a fake
# port. run.sh compares them.
#
#   tests/eventloop/run.sh [build dir]
project(nanostack-eventloop-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

foreach(variant periodic skip skip_slots)
    add_executable(test_eventloop_${variant}
        test_idle_ticks.c
        ${ROOT}/nanostack-event-loop/source/event.c
        ${ROOT}/nanostack-event-loop/source/system_timer.c
        ${ROOT}/mbed-client-libservice/source/libList/ns_list.c
    )
    target_include_directories(test_eventloop_${variant} PRIVATE
        ${ROOT}/include
        ${ROOT}/nanostack-event-loop
        ${ROOT}/nanostack-event-loop/source
        ${ROOT}/mbed-client-libservice
    )
    target_compile_options(test_eventloop_${variant} PRIVATE -std=gnu17 -Wall -Wno-sign-compare)
endforeach()
# skip: a port with µs timers (NS_PORT_TIMER_USEC), as the POSIX and
# simulation ports; skip_slots: 50 µs slots only, as the FG25 port
target_compile_definitions(test_eventloop_skip PRIVATE NS_EVENTLOOP_SKIP_IDLE_TICKS NS_PORT_TIMER_USEC)
target_compile_definitions(test_eventloop_skip_slots PRIVATE NS_EVENTLOOP_SKIP_IDLE_TICKS)
