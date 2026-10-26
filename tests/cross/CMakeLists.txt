cmake_minimum_required(VERSION 3.17)
# Cross-build check (GENERICITY_PLAN.md step S3).
#
# Builds the stack for a Cortex-M33 the way a firmware consumes it: through
# add_subdirectory(), with its own port target (a stub), mbedTLS supplied by
# the consumer, the MAC in the same image and no stdio file system. Then it
# links a border router and a router image that follow the init sequence of
# §3.3. Nothing runs: the check is that the stack builds without POSIX and
# that no symbol is missing.
#
#   tests/cross/run.sh <mbedtls v3.0.0 source tree>
project(nanostack-cross-check C)

set(MBEDTLS_DIR "" CACHE PATH "Mbed TLS v3.0.0 source tree")
if(NOT EXISTS "${MBEDTLS_DIR}/library/ssl_tls.c")
    message(FATAL_ERROR "Set MBEDTLS_DIR to an Mbed TLS v3.0.0 source tree")
endif()

# The inherited code needs these with GCC >= 14 (README).
add_compile_options(-std=gnu17 -fpermissive)

# mbedTLS: default configuration, minus what needs an OS (mbedtls_cross_config.h).
file(GLOB MBEDTLS_SOURCES "${MBEDTLS_DIR}/library/*.c")
add_library(mbedtls_cross STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtls_cross PUBLIC "${MBEDTLS_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(mbedtls_cross PRIVATE "${MBEDTLS_DIR}/library")
target_compile_definitions(mbedtls_cross PUBLIC MBEDTLS_USER_CONFIG_FILE="mbedtls_cross_config.h")
target_compile_options(mbedtls_cross PRIVATE -w)
add_library(MbedTLS::mbedtls ALIAS mbedtls_cross)
add_library(MbedTLS::mbedcrypto ALIAS mbedtls_cross)
add_library(MbedTLS::mbedx509 ALIAS mbedtls_cross)

# A port that implements the porting API with stubs.
add_library(stub_port STATIC stub_port.c)
target_link_libraries(stub_port PUBLIC nanostack_port_api nanostack_eventloop)
target_include_directories(stub_port PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../nanostack)

set(NANOSTACK_PORT stub_port CACHE STRING "" FORCE)
set(NANOSTACK_FILE_NO_STDIO ON CACHE BOOL "" FORCE)
add_subdirectory(../.. nanostack)
nanostack_add_library(ns_br CONFIG ws_border_router BORDER_ROUTER)
nanostack_add_library(ns_rn CONFIG ws_router)
nanostack_add_rcp_library(ns_mac INPROCESS)
foreach(t ns_br ns_rn ns_mac nanostack_eventloop nanostack_libservice)
    target_compile_options(${t} PRIVATE -w)
endforeach()

foreach(role br rn)
    add_executable(link_${role} main.c)
    target_compile_definitions(link_${role} PRIVATE ROLE_$<UPPER_CASE:${role}>=1)
    target_link_libraries(link_${role} PRIVATE
        -Wl,--start-group ns_${role} ns_mac nanostack_eventloop nanostack_libservice
        stub_port mbedtls_cross -Wl,--end-group)
    target_link_options(link_${role} PRIVATE -Wl,--gc-sections -Wl,-Map=link_${role}.map)
endforeach()
