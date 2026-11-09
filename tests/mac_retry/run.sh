#!/bin/sh
# Backoff of a MAC retry after a missing ACK (GS12, docs/MAC_RETRY.md):
# configure, build and run both variants (the fix, and upstream, which must
# show the lock-step). Exit status 0 if every check passes.
#   tests/mac_retry/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-mac-retry-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_mac_retry_upstream"
"$build/test_mac_retry_fix"
