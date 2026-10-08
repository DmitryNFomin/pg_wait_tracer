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
    # The Mac-side bridge FIRST, and with SIGTERM to the process holding the
    # port -- web/main.go's shutdown path is what runs bridge.Close(), which is
    # what reaps the pgwt-server it started over ssh ON THE BOX. Skipping this
    # (as the 2026-10-08 run did) leaves an orphaned
    # `pgwt-server /root/paint1_trace_r1` resident on a shared box with the
    # trace dir open. Found by hand after the run; killing the bridge reaped it
    # exactly as designed, which is the whole reason to go through the bridge
    # rather than pkill the server directly.
    local holder
    holder=$(lsof -nP -iTCP:8384 -sTCP:LISTEN -t 2>/dev/null)
    if [ -n "$holder" ]; then
        echo "  bridge on 8384: $(ps -p "$holder" -o args= | cut -c1-90)"
        kill "$holder" 2>/dev/null
        for _ in $(seq 1 20); do
            lsof -nP -iTCP:8384 -sTCP:LISTEN -t >/dev/null 2>&1 || break
            sleep 1
        done
        if lsof -nP -iTCP:8384 -sTCP:LISTEN -t >/dev/null 2>&1; then
            echo "  WARNING: port 8384 still held after TERM"
        else
            echo "  bridge stopped, port 8384 free"
        fi
    fi
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
        # The lock holder is \`bash -c 'echo held; sleep 2700'\` and its
        # \`sleep\` CHILD, and the child inherits fd 9 -- so killing only the
        # bash (which is what \`pgrep -f 'sleep 2700' | head -1\` returns,
        # because the wrapper's own command line contains that string)
        # leaves the sleep holding the flock and every later box-check
        # queueing behind it. Observed on 2026-10-08: box-check run 2 sat on
        # \"waiting for /tmp/pgwt-box-check.lock\" until the orphaned sleep
        # was killed by hand. So: identify holders by the FD, not by a
        # command-line pattern, and verify the lock is actually free.
        for lh in \$(fuser /tmp/pgwt-box-check.lock 2>/dev/null); do
            kill \$lh 2>/dev/null && echo \"lock holder \$lh signalled\"
        done
        sleep 3
        for lh in \$(fuser /tmp/pgwt-box-check.lock 2>/dev/null); do
            kill -9 \$lh 2>/dev/null && echo \"lock holder \$lh KILLed\"
        done
        sleep 1
        if fuser /tmp/pgwt-box-check.lock >/dev/null 2>&1; then
            echo 'ERROR: /tmp/pgwt-box-check.lock is STILL held after teardown:'
            fuser -v /tmp/pgwt-box-check.lock 2>&1
        else
            echo 'lock free'
        fi
        # pgwt-server too: if the bridge shutdown above did not reap it (a
        # KILLed bridge never calls bridge.Close()), it is a resident process
        # on a shared box holding the trace dir open. \$\$ is excluded so this
        # shell does not match its own command line and kill itself.
        for p in \$(pgrep -f 'pgwt-server /root/paint1_trace'); do
            [ \"\$p\" = \"\$\$\" ] && continue
            kill \$p 2>/dev/null && echo \"orphaned pgwt-server \$p killed\"
        done
        ps -eo pid,args | grep -E 'burst_load.sh|live_loop_workload|pg_wait_tracer --daemon|pgwt-server /root/paint1' | grep -v 'grep -E' || echo 'no load/daemon/server left'
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
