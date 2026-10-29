#!/bin/sh
# Protocol timers, periodic against idle-tick skipping (docs/PORTING.md,
# "Idle-tick skipping of the protocol timers"): configure, build, run the
# three variants on the same schedule and compare what the fake modules did
# and saw. Exit status 0 if the three outputs are identical and each variant
# passes its own checks.
#   tests/protocol_timers/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-protocol-timers-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
for v in periodic skip skip_slots; do
    "$build/test_protocol_timers_$v" > "$build/$v.txt" 2> "$build/$v.err" || { cat "$build/$v.err" >&2; exit 1; }
    tail -1 "$build/$v.err"
done
for v in skip skip_slots; do
    if ! cmp -s "$build/periodic.txt" "$build/$v.txt"; then
        echo "protocol_timers: periodic and $v differ:" >&2
        diff "$build/periodic.txt" "$build/$v.txt" | head -5 >&2
        exit 1
    fi
done
echo "protocol_timers: same $(wc -l < "$build/periodic.txt") lines (module actions and observations) with the periodic timers and with skipping (µs timers and 50 µs slots)"
