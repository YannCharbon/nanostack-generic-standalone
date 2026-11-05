#!/bin/sh
# The LFN's receive windows in the MAC: configure, build and run the unit
# test. Exit status 0 if every check passes.
#   tests/mac_window/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-mac-window-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_mac_window"
