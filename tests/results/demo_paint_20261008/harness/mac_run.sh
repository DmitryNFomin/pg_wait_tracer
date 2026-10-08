#!/usr/bin/env bash
# Mac side: one complete run. Waits for the box's capture to reach a FULL 900 s
# window (polling the box's own clock, not a local sleep -- a half-filled
# window understates paint cost), points the bridge at that run's trace dir,
# walks 11 tabs x 3 navigations in real Chrome, then records the conditions and
# stamps them into the walk JSON.
#
# Never asserts anything it could not read: workload_alive is written as the
# literal result of asking the box, and paint_analyse.py refuses the walk if
# it is anything but true.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WT=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-afa9ff9486d78f624
EV="$WT/tests/results/demo_paint_20261008"
BOX=root@2.28.45.47
RUN="${1:?usage: mac_run.sh RUNLABEL}"
MIN_AGE=${MIN_AGE:-900}

echo "=== $RUN: waiting for a full ${MIN_AGE}s window ==="
while :; do
    age=$(ssh -o ConnectTimeout=10 "$BOX" \
        ". /tmp/paint1_run_$RUN.env && echo \$(( \$(date +%s) - START_EPOCH ))" 2>/dev/null)
    case "$age" in
        ''|*[!0-9]*) echo "could not read capture age (got '$age'); retrying"; sleep 15; continue ;;
    esac
    echo "  capture age ${age}s"
    [ "$age" -ge "$MIN_AGE" ] && break
    sleep 30
done

echo "=== $RUN: bridge ==="
bash "$HERE/mac_bridge.sh" "$RUN" || exit 1

echo "=== $RUN: walk ==="
python3 "$EV/paint_walk2.py" --label "$RUN" --out "$EV/walk-$RUN.json" \
    2>&1 | tee "$HERE/walk-$RUN.log"
rc=${PIPESTATUS[0]}
echo "walk exit=$rc"

echo "=== $RUN: conditions ==="
ssh -o ConnectTimeout=10 "$BOX" "bash /root/pgwt-paint-20261008/box_run_conditions.sh $RUN" \
    2>&1 | tee "$EV/conditions-$RUN.txt"

echo "=== $RUN: stamping conditions into the walk JSON ==="
python3 "$HERE/stamp.py" "$EV/walk-$RUN.json" "$EV/conditions-$RUN.txt"
echo "RUN_$RUN-DONE rc=$rc"
