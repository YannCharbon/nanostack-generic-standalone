/* Mbed TLS v3.0.0 user configuration for the cross-build check: the default
 * configuration without what needs an OS. The stack's needs are listed in
 * GENERICITY_PLAN.md D12; D14 asks for no time/date checks on targets.
 * SPDX-License-Identifier: Apache-2.0
 */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE
