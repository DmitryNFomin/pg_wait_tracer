#!/bin/sh
# run_redgreen.sh — runs redprobe327 for all six #327 sites against the
# PARENT compute.c and against the FIXED one, under a hard timeout.
#
# Pre-fix the expected outcome is exit 124 (timeout killed it) with no
# RETURNED line: the probe loop never terminates. Post-fix every site must
# print RETURNED and exit 0.
#
# usage: run_redgreen.sh <parent-binary> <fixed-binary> [timeout-seconds]
set -u
PARENT="$1"
FIXED="$2"
T="${3:-30}"
SITES="sessions sessions_summary queries queries_summary fingerprints variants"

printf '%s\n' "=== #327 red/green, timeout ${T}s per site ==="
printf '%s\n' "parent binary: $PARENT"
printf '%s\n' "fixed  binary: $FIXED"

for which in parent fixed; do
    case "$which" in
        parent) BIN="$PARENT" ;;
        fixed)  BIN="$FIXED" ;;
    esac
    printf '\n--- %s (%s) ---\n' "$which" "$BIN"
    for s in $SITES; do
        out=$(timeout "$T" "$BIN" "$s" 2>&1)
        rc=$?
        if [ "$rc" -eq 124 ]; then
            verdict="RED: did not terminate in ${T}s (the probe loop spins)"
        elif [ "$rc" -eq 0 ]; then
            verdict="GREEN: $(printf '%s' "$out" | grep RETURNED)"
        else
            verdict="ERROR: exit $rc — $out"
        fi
        printf '  %-18s exit=%-4s %s\n' "$s" "$rc" "$verdict"
    done
done
