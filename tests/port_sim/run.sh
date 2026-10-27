#!/bin/sh
# Unit tests of the simulation port: configure, build and run.
#   tests/port_sim/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-port-sim-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_port_sim"
