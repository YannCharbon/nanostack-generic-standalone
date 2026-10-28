#!/bin/sh
# Event loop timers, periodic tick against idle-tick skipping (docs/PORTING.md,
# "Idle-tick skipping"): configure, build, run both on the same schedule and
# compare the events
# they deliver (time and ID). Exit status 0 if they are identical (and the
# 50 µs-slot variant passes its own checks).
#   tests/eventloop/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-eventloop-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
"$build/test_eventloop_periodic" > "$build/periodic.txt"
"$build/test_eventloop_skip" > "$build/skip.txt"
# Its own checks only: its ticks may land up to 50 µs late, which changes
# the order against the schedule's other actions
"$build/test_eventloop_skip_slots" > "$build/skip_slots.txt"
if cmp -s "$build/periodic.txt" "$build/skip.txt"; then
    echo "eventloop: same $(wc -l < "$build/skip.txt") deliveries (time, event) with the periodic tick and with idle-tick skipping"
else
    echo "eventloop: deliveries differ:" >&2
    diff "$build/periodic.txt" "$build/skip.txt" | head -5 >&2
    exit 1
fi
