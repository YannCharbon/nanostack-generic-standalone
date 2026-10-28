#!/bin/sh
# FAN 1.1 LFN unicast mechanics in the FHSS (docs/LFN_MAC.md): configure,
# build and run the unit test. Exit status 0 if every check passes.
#   tests/fhss_lfn/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-fhss-lfn-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_fhss_lfn"
