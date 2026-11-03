#!/bin/sh
# LFN code outside the FHSS: configure, build and run the unit test. Exit
# status 0 if every check passes.
#   tests/lfn_units/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-lfn-units-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_lfn_units"
