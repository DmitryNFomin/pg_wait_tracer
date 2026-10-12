#!/bin/sh
# run_redgreen.sh — runs redprobe327 for all six #327 sites against the
# PARENT compute.c and against the FIXED one, under a hard timeout.
#
# Pre-fix the expected outcome is exit 124 (timeout killed it) with no
# RETURNED line: the probe loop never terminates. Post-fix every site must
# print RETURNED and exit 0.
#
# This script CHECKS what it observes and exits non-zero otherwise, so the
# evidence file cannot be a page of "ERROR: exit 127" that still looks like a
# clean run: every parent site must be 124, every fixed site must be 0 AND
# print a RETURNED line, and anything else (missing `timeout`, a binary that
# is not executable, a crash, a parent that returns) is a failure. Absent
# evidence is never approval.
#
# usage: run_redgreen.sh <parent-binary> <fixed-binary> [timeout-seconds]
set -u
PARENT="$1"
FIXED="$2"
T="${3:-30}"
SITES="sessions sessions_summary queries queries_summary fingerprints variants"
bad=0

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
        ret=$(printf '%s' "$out" | grep RETURNED)
        if [ "$which" = parent ]; then
            if [ "$rc" -eq 124 ]; then
                verdict="RED: did not terminate in ${T}s (the probe loop spins)"
            else
                verdict="NO RED: expected exit 124, got $rc — $out"
                bad=1
            fi
        else
            if [ "$rc" -eq 0 ] && [ -n "$ret" ]; then
                verdict="GREEN: $ret"
            else
                verdict="NO GREEN: expected exit 0 with a RETURNED line, got $rc — $out"
                bad=1
            fi
        fi
        printf '  %-18s exit=%-4s %s\n' "$s" "$rc" "$verdict"
    done
done

if [ "$bad" -ne 0 ]; then
    printf '\n%s\n' "RED/GREEN EVIDENCE REFUSED: see the NO RED / NO GREEN lines above."
    exit 1
fi
printf '\n%s\n' "all six sites: parent RED (124), fixed GREEN (0, RETURNED)."
