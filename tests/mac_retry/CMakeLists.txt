cmake_minimum_required(VERSION 3.17)
# Unit test of the backoff of a MAC retry after a missing ACK with FHSS
# (GS12: mac_pd_sap.c, mac_csma_backoff_get(); mac_retry_backoff.h;
# docs/MAC_RETRY.md), with a fake FHSS hold and scripted random draws. Two
# variants: the fix (NS_MAC_RETRY_SUN_BACKOFF) and upstream, which must show
# the lock-step.
#
#   tests/mac_retry/run.sh [build dir]
project(nanostack-mac-retry C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
foreach(v fix upstream)
    add_executable(test_mac_retry_${v}
        test_mac_retry.c
        stubs.c
        ${ROOT}/rcp/mac/ieee802154/mac_pd_sap.c
    )
    target_include_directories(test_mac_retry_${v} PRIVATE
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
    target_compile_definitions(test_mac_retry_${v} PRIVATE NSCONFIG=ws_router)
    target_compile_options(test_mac_retry_${v} PRIVATE -std=gnu17 -Wall -Wno-sign-compare -fpermissive)
    target_link_libraries(test_mac_retry_${v} PRIVATE m)
endforeach()
target_compile_definitions(test_mac_retry_fix PRIVATE NS_MAC_RETRY_SUN_BACKOFF)
