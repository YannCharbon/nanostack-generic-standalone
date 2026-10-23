#!/bin/sh
# Cross-build check (GENERICITY_PLAN.md S3): configure, build and link the
# stack for a Cortex-M33 with a stub port. Needs arm-none-eabi-gcc in PATH,
# CMake and Ninja.
#   tests/cross/run.sh <mbedtls v3.0.0 source tree> [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
mbedtls=$(cd "$1" && pwd)
build=${2:-$here/../../build-cross}
cmake -S "$here" -B "$build" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$here/arm-none-eabi.cmake" -DMBEDTLS_DIR="$mbedtls"
cmake --build "$build"
arm-none-eabi-size "$build/link_br" "$build/link_rn"
