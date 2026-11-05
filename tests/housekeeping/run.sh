#!/bin/sh
# Housekeeping timers with nothing to do (docs/PORTING.md, "Housekeeping with
# nothing to do"): the stack's neighbour and destination caches, MLD's query
# timer and the blacklist, run with the periodic 1 s timer and with the
# skipping core timer's calls, on the same schedule. Exit status 0 if every
# output is identical and each variant passes its own checks.
#   tests/housekeeping/run.sh [build dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../../build-housekeeping-tests}
cmake -S "$here" -B "$build" -G Ninja >/dev/null
cmake --build "$build"
for v in periodic skip_prev skip skip_ffn; do
    "$build/test_housekeeping_$v" > "$build/$v.txt" 2> "$build/$v.err" || { cat "$build/$v.err" >&2; exit 1; }
    tail -1 "$build/$v.err"
done
for v in skip_prev skip skip_ffn; do
    if ! cmp -s "$build/periodic.txt" "$build/$v.txt"; then
        echo "housekeeping: periodic and $v differ:" >&2
        diff "$build/periodic.txt" "$build/$v.txt" | head -10 >&2
        exit 1
    fi
done
echo "housekeeping: same $(wc -l < "$build/periodic.txt") lines (actions, random draws, states read by events) with the periodic timer and with skipping (DI6 only, with housekeeping skipping as an LFN and as an FFN)"
