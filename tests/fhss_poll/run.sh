#!/bin/sh
# FHSS TX queue poll, periodic against idle-poll skipping (docs/PORTING.md,
# "Idle-poll skipping of the FHSS TX queue"): configure, build, run both
# variants on the same schedule and compare their polls. Exit status 0 if
# the two outputs are identical and each variant passes its own checks.
#   tests/fhss_poll/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-fhss-poll-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
for v in periodic skip; do
    "$build/test_fhss_poll_$v" > "$build/$v.txt" 2> "$build/$v.err" || { cat "$build/$v.err" >&2; exit 1; }
    tail -1 "$build/$v.err"
done
if ! cmp -s "$build/periodic.txt" "$build/skip.txt"; then
    echo "fhss_poll: periodic and skip differ:" >&2
    diff "$build/periodic.txt" "$build/skip.txt" | head -5 >&2
    exit 1
fi
echo "fhss_poll: same $(wc -l < "$build/periodic.txt") lines (polls, frames, state changes) with the periodic poll and with skipping"
