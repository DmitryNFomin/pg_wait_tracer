#!/usr/bin/env bash
# One paint run end to end, on gate-1, with the 2026-10-08 three-tier load.
#
#   1. box_setup.sh      takes /tmp/pgwt-box-check.lock (flock -n: refuses a
#                        contended box rather than queueing silently), builds,
#                        starts pgbench + live_loop_workload + burst_load
#   2. box_run_start.sh  a FRESH EMPTY trace dir + its own --mode full daemon,
#                        refusing if any load tier (burst included) is dead
#   3. mac_chrome.sh     the demo Chrome profile with a CDP port
#   4. mac_run.sh        polls the BOX's clock for a full 900 s window, points
#                        the bridge at that trace dir, walks 11 tabs x 3
#                        navigations, records the box conditions, stamps them
#                        into the walk JSON
#   5. paint_analyse.py  the verdict -- refuses an incomplete walk instead of
#                        taking the max over whatever happened to work
#   6. teardown          daemon, load, and the lock holder, all by pid
#
# Usage: bash run_chain.sh [RUNLABEL]   (default r1)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EV="$(dirname "$HERE")"
BOX=root@2.28.45.47
ROOT=/root/pgwt-paint-20261008
RUN="${1:-r1}"

teardown() {
    echo "=== teardown ==="
    ssh -o ConnectTimeout=10 "$BOX" "
        . /tmp/paint1_run_$RUN.env 2>/dev/null
        . /tmp/paint1_pids.env 2>/dev/null
        for p in \$DAEMON_PID \$BURST_PID \$WORKLOAD_PID \$PGBENCH_PID; do
            [ -n \"\$p\" ] && kill \$p 2>/dev/null
        done
        sleep 6
        for p in \$DAEMON_PID \$BURST_PID \$WORKLOAD_PID \$PGBENCH_PID; do
            [ -n \"\$p\" ] && kill -0 \$p 2>/dev/null && echo \"still up: \$p\" || true
        done
        lh=\$(pgrep -f 'sleep 2700' | head -1)
        [ -n \"\$lh\" ] && { kill \$lh; echo \"lock holder \$lh killed\"; }
        pgrep -af 'burst_load.sh|live_loop_workload|pg_wait_tracer --daemon' || echo 'no load/daemon left'
    " 2>&1 | sed 's/^/  /'
}

echo "=== 1. box setup (lock, build, three load tiers) ==="
ssh -o ConnectTimeout=10 "$BOX" "bash $ROOT/box_setup.sh" || { echo "SETUP FAILED"; teardown; exit 1; }

echo "=== 2. fresh capture for $RUN ==="
ssh -o ConnectTimeout=10 "$BOX" "bash $ROOT/box_run_start.sh $RUN" || { echo "RUN START FAILED"; teardown; exit 1; }

echo "=== 3. Chrome ==="
bash "$HERE/mac_chrome.sh" || { echo "CHROME FAILED"; teardown; exit 1; }

echo "=== 4. fill + walk ==="
bash "$HERE/mac_run.sh" "$RUN"
walk_rc=$?
echo "mac_run rc=$walk_rc"

echo "=== 5. verdict ==="
python3 "$EV/paint_analyse.py" "$EV/walk-$RUN.json" | tee "$EV/verdict-$RUN.txt"
verdict_rc=${PIPESTATUS[0]}

teardown
echo "CHAIN DONE walk_rc=$walk_rc verdict_rc=$verdict_rc"
exit "$verdict_rc"
