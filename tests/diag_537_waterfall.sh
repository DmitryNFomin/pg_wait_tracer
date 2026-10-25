#!/bin/bash
# diag_537_waterfall.sh — issue #537 DIAGNOSTIC. NOT A GATE, not wired into
# tests/run_all.sh, and it writes nothing run_all.sh or the rehearsal reads.
#
# Brings up the SAME live stack tests/ui_live_smoke.sh does (pgbench + the
# looping lock workload + pg_wait_tracer --daemon --mode full + the Go
# bridge) and runs tests/diag_537_waterfall.py against it TWICE: once while
# the capture is young and once after it has grown. Two phases because the
# one mechanism the existing evidence cannot distinguish is dataset age --
# ui_live_smoke.sh's own comment records that an unthrottled 4-client full-mode
# pgbench grew the executions dataset until "Waterfall's latest-execution
# query stopped answering within the 60s no-data budget by the time the walk
# reached it (~15-20 min in)", and #514's three failures were measured late in
# a run whose early walk had reached the same tab in 480 ms. A single-phase
# sweep would answer #537 with whichever phase it happened to sample.
#
# Reverse-order teardown, and it refuses rather than reporting a clean sweep
# if either phase could not see (the python side exits 2 on VOID).
#
# Usage: sudo tests/diag_537_waterfall.sh [--pid PM_PID] [--pg-version N]
#          [--attempts N] [--early-age-s N] [--late-age-s N]
# Must be wrapped in `flock /tmp/pgwt-box-check.lock` on a shared box.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"
source "$SCRIPT_DIR/live_daemon_lib.sh"

TRACER="$PROJECT_DIR/pg_wait_tracer"
SERVER="$PROJECT_DIR/pgwt-server"
BRIDGE="$PROJECT_DIR/web/pgwt"

PM_PID=""; PG_MAJOR=""
ATTEMPTS="${PGWT_DIAG537_ATTEMPTS:-10}"
EARLY_AGE_S="${PGWT_DIAG537_EARLY_AGE_S:-120}"
LATE_AGE_S="${PGWT_DIAG537_LATE_AGE_S:-1200}"
PORT="${PGWT_DIAG537_PORT:-8537}"
OUT_DIR="$SCRIPT_DIR/results/diag_537"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_MAJOR="$2"; shift 2 ;;
        --attempts) ATTEMPTS="$2"; shift 2 ;;
        --early-age-s) EARLY_AGE_S="$2"; shift 2 ;;
        --late-age-s) LATE_AGE_S="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid PID] [--pg-version N] [--attempts N] [--early-age-s N] [--late-age-s N]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_MAJOR" ]]; then PM_PID=$(find_postmaster --pg-version "$PG_MAJOR")
    else PM_PID=$(find_postmaster); fi
fi
[[ -n "$PM_PID" ]] || { echo "ERROR: cannot find postmaster PID"; exit 1; }
derive_pgport "$PM_PID" "$PG_MAJOR" "diag_537"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    [[ -x "$bin" ]] || { echo "ERROR: $bin not built"; exit 1; }
done

# The stack must outlast BOTH phases plus every attempt's own 30s budget.
# 2 paths * ATTEMPTS * 30s worst case per phase, plus the late-age wait.
DURATION_S=$(( LATE_AGE_S + 4 * ATTEMPTS * 30 + 300 ))
echo "diag_537: stack duration ${DURATION_S}s, attempts/path=$ATTEMPTS, phases at ${EARLY_AGE_S}s and ${LATE_AGE_S}s"

TRACE_DIR=$(mktemp -d /tmp/pgwt_diag537_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_diag537_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_diag537_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_diag537_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_diag537_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""; BRIDGE_PID=""; WORKLOAD_PID=""; PGBENCH_PID=""

cleanup() {
    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10 INT
    stop_pid "$TRACER_PID" 15
    echo "diag_537: daemon log tail:";  tail -n 25 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "diag_537: bridge log tail:";  tail -n 25 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    sleep 1
    local leftover; leftover="$(pgrep -f "$TRACE_DIR" 2>/dev/null || true)"
    rm -rf "$TRACE_DIR"; rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"
    if [[ -n "$leftover" ]]; then
        echo "ERROR: processes from this run survived teardown: $leftover"
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check_pgbench_provisioned "diag_537" "$PGBENCH_LOG"
echo "diag_537: starting pgbench (4 clients, --rate=25, ${DURATION_S}s)"
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" --rate=25 >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!
sleep 1
kill -0 "$PGBENCH_PID" 2>/dev/null || { echo "ERROR: pgbench died"; tail -20 "$PGBENCH_LOG"; exit 1; }

echo "diag_537: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2
kill -0 "$WORKLOAD_PID" 2>/dev/null || { echo "ERROR: workload died"; tail -40 "$WORKLOAD_LOG"; exit 1; }

echo "diag_537: starting daemon (--mode full, $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" --mode full -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!
for _ in $(seq 1 60); do
    [[ -S "$SOCK" ]] && break
    kill -0 "$TRACER_PID" 2>/dev/null || { echo "ERROR: daemon exited"; tail -40 "$DAEMON_LOG"; exit 1; }
    sleep 0.5
done
[[ -S "$SOCK" ]] || { echo "ERROR: control socket never appeared"; tail -40 "$DAEMON_LOG"; exit 1; }
CAPTURE_START=$(date +%s)
echo "diag_537: daemon up at epoch $CAPTURE_START"

echo "diag_537: starting Go bridge on :$PORT"
"$BRIDGE" --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER" \
    root@localhost >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!
BASE_URL="http://localhost:$PORT/"
for _ in $(seq 1 60); do
    url_ready "${BASE_URL}session" && break
    kill -0 "$BRIDGE_PID" 2>/dev/null || { echo "ERROR: bridge exited"; tail -40 "$BRIDGE_LOG"; exit 1; }
    sleep 0.5
done
url_ready "${BASE_URL}session" || { echo "ERROR: bridge never answered"; tail -40 "$BRIDGE_LOG"; exit 1; }
echo "diag_537: bridge ready at $BASE_URL"

# Same refusal ui_live_smoke.sh/demo_rehearsal.sh use: a stale web/pgwt
# embeds old web/static and would measure the wrong assets.
assert_served_ui_matches_tree "$BASE_URL" "$PROJECT_DIR/web/static" "diag_537" || {
    echo "ERROR: bridge is not serving this tree's web/static -- rebuild with 'make pgwt-client'"; exit 1; }

mkdir -p "$OUT_DIR"
RC=0
run_phase() {
    local phase="$1" target_age="$2"
    local now age
    now=$(date +%s); age=$(( now - CAPTURE_START ))
    if (( age < target_age )); then
        echo "diag_537: waiting $(( target_age - age ))s for capture age ${target_age}s (phase $phase)"
        sleep $(( target_age - age ))
    fi
    now=$(date +%s); age=$(( now - CAPTURE_START ))
    echo "diag_537: ══ phase $phase at capture age ${age}s ══"
    python3 "$SCRIPT_DIR/diag_537_waterfall.py" --url "$BASE_URL" \
        --attempts "$ATTEMPTS" --phase "$phase" \
        --out "$OUT_DIR/$phase.json" --out-dir "$OUT_DIR/$phase"
    local rc=$?
    echo "diag_537: phase $phase rc=$rc (capture age at start ${age}s)"
    [[ $rc -ne 0 ]] && RC=$rc
}

run_phase early "$EARLY_AGE_S"
run_phase late "$LATE_AGE_S"

echo "diag_537: artifacts in $OUT_DIR"
exit "$RC"
