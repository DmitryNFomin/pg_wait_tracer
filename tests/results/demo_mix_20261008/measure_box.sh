#!/bin/bash
# Runs ON the gate box, inside the box-check flock. Captures a WINDOW_S window
# with the full demo load (pgbench + live_loop_workload + burst_load) under a
# --mode full daemon, then dumps the wait-event mix, AAS and Transitions node
# count as JSON to /tmp/pgwt_measure_out/.
set -uo pipefail
cd "$1" || exit 1
WINDOW_S="${2:-900}"
PGV="${3:-18}"
OUT=/tmp/pgwt_measure_out
rm -rf "$OUT"; mkdir -p "$OUT"

source tests/testutil.sh
source tests/live_daemon_lib.sh

PM_PID=$(find_postmaster --pg-version "$PGV")
[[ -n "$PM_PID" ]] || { echo "no postmaster for PG$PGV"; exit 1; }
derive_pgport "$PM_PID" "$PGV" "measure"
export PGPORT

echo "measure: PM_PID=$PM_PID PGPORT=$PGPORT window=${WINDOW_S}s"

TRACE=$(mktemp -d /tmp/pgwt_measure_XXXXXX)
DLOG="$OUT/daemon.log"; BLOG="$OUT/burst.log"
WLOG="$OUT/workload.log"; PLOG="$OUT/pgbench.log"
TRACER_PID=""; PGB_PID=""; WL_PID=""; BU_PID=""

cleanup() {
    for p in "$BU_PID" "$WL_PID" "$PGB_PID"; do stop_pid "$p" 20; done
    stop_pid "$TRACER_PID" 20
    rm -rf "$TRACE"
}
trap cleanup EXIT

# 1. daemon first, so the whole load window is observed
./pg_wait_tracer --daemon --pid "$PM_PID" -i 1 -T "$TRACE" --mode full -v \
    >/dev/null 2>"$DLOG" &
TRACER_PID=$!
for _ in $(seq 1 60); do [[ -S "$TRACE/pgwt.sock" ]] && break; sleep 0.5; done
[[ -S "$TRACE/pgwt.sock" ]] || { echo "no control socket"; tail -20 "$DLOG"; exit 1; }
echo "measure: daemon up"

# 2. the three load tiers, exactly as tests/ui_live_smoke.sh starts them
pgbench -U postgres -d postgres -c 4 -T "$WINDOW_S" --rate=25 >>"$PLOG" 2>&1 &
PGB_PID=$!
python3 tests/live_loop_workload.py "$WINDOW_S" >>"$WLOG" 2>&1 &
WL_PID=$!
PGWT_BURST_LOG="$BLOG" bash tests/burst_load.sh "$WINDOW_S" >>"$BLOG" 2>&1 &
BU_PID=$!
sleep 3
for nm in pgbench:$PGB_PID workload:$WL_PID burst:$BU_PID; do
    p=${nm#*:}
    kill -0 "$p" 2>/dev/null || { echo "FATAL: ${nm%%:*} died at startup"; tail -30 "$BLOG" "$WLOG" "$PLOG"; exit 1; }
done
echo "measure: all three load tiers alive"

# 3. wait out the window, checking the burst tier has not refused
end=$(( $(date +%s) + WINDOW_S + 20 ))
while [[ $(date +%s) -lt $end ]]; do
    sleep 30
    if ! kill -0 "$BU_PID" 2>/dev/null; then
        wait "$BU_PID"; rc=$?
        if [[ $rc -ne 0 ]]; then
            echo "FATAL: burst_load refused (exit $rc) -- this capture is not the demo's load"
            tail -40 "$BLOG"; exit 1
        fi
        BU_PID=""
    fi
    if [[ -n "$WL_PID" ]] && ! kill -0 "$WL_PID" 2>/dev/null; then
        wait "$WL_PID"; rc=$?
        [[ $rc -eq 0 ]] || { echo "FATAL: live_loop_workload exited $rc"; tail -40 "$WLOG"; exit 1; }
        WL_PID=""
    fi
done
echo "measure: window done"

for p in "$BU_PID" "$WL_PID" "$PGB_PID"; do stop_pid "$p" 20; done
BU_PID=""; WL_PID=""; PGB_PID=""
sleep 3
stop_pid "$TRACER_PID" 25
TRACER_PID=""
sleep 2

# 4. query the trace
python3 tests/../tests/measure_query.py "$TRACE" "$OUT" || exit 1
cp -r "$TRACE" "$OUT/trace" 2>/dev/null || true
du -sh "$OUT/trace" 2>/dev/null
echo "measure: OK"
