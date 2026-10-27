cmake_minimum_required(VERSION 3.17)
# Unit tests of the simulation port (port/sim, VIRTUAL_TIME.md §4).
#
# Builds the port's sources with the POSIX port files it shares and a stub
# event loop, without the stack and without mbedTLS, then runs each test
# against a fake conductor over socketpair().
#
#   tests/port_sim/run.sh [build dir]
project(nanostack-port-sim-tests C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_port_sim
    test_port_sim.c
    ${ROOT}/port/sim/sim_fhss_timer.c
    ${ROOT}/port/sim/sim_random.c
    ${ROOT}/port/sim/sim_scheduler.c
    ${ROOT}/port/sim/sim_stdio.c
    ${ROOT}/port/sim/sim_time.c
    ${ROOT}/port/sim/sim_timer.c
    ${ROOT}/port/posix/common/log.c
    ${ROOT}/port/posix/common/slist.c
)
target_include_directories(test_port_sim PRIVATE
    ${ROOT}/include
    ${ROOT}/nanostack-event-loop
    ${ROOT}/nanostack
    ${ROOT}/port/posix
    ${ROOT}/port/sim
)
target_compile_definitions(test_port_sim PRIVATE NS_PORT_SIM)
target_compile_options(test_port_sim PRIVATE -std=gnu17 -Wall -Wno-sign-compare)

enable_testing()
add_test(NAME port_sim COMMAND test_port_sim)
