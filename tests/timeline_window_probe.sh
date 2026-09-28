#!/bin/bash
# timeline_window_probe.sh -- issue #205 measurement harness.
#
# Brings up the SAME live stack tests/ui_live_smoke.sh uses (pgbench + the
# looping lock/sleep workload + a --mode full daemon + the Go bridge) and runs
# tests/timeline_window_probe.py against it. It answers one question -- does
# the timeline's live window advance per tick, and does the paint follow --
# and is deliberately NOT wired into run_all.sh: it is a measurement, not a
# gate.
#
# Must be run under the box's shared lock, like every other CPU-using step:
#   flock /tmp/pgwt-box-check.lock sudo tests/timeline_window_probe.sh --pg-version 13
#
# Usage: sudo tests/timeline_window_probe.sh [--pid PID] [--pg-version N]
#                                            [--ticks N] [--runs N]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"
source "$SCRIPT_DIR/live_daemon_lib.sh"

TRACER="$PROJECT_DIR/pg_wait_tracer"
SERVER="$PROJECT_DIR/pgwt-server"
BRIDGE="$PROJECT_DIR/web/pgwt"

PM_PID=""
PG_MAJOR=""
TICKS=8
RUNS=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_MAJOR="$2"; shift 2 ;;
        --ticks) TICKS="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid PID] [--pg-version N] [--ticks N] [--runs N]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_MAJOR" ]]; then PM_PID=$(find_postmaster --pg-version "$PG_MAJOR")
    else PM_PID=$(find_postmaster); fi
fi
[[ -z "$PM_PID" ]] && { echo "ERROR: cannot find postmaster PID"; exit 1; }
echo "probe: postmaster PID $PM_PID"
derive_pgport "$PM_PID" "$PG_MAJOR" "timeline_window_probe"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    [[ -x "$bin" ]] || { echo "ERROR: $bin not built"; exit 1; }
done

DURATION_S="${PGWT_PROBE_DURATION_S:-420}"
PORT="${PGWT_PROBE_PORT:-8387}"

TRACE_DIR=$(mktemp -d /tmp/pgwt_probe_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_probe_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_probe_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_probe_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_probe_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"

TRACER_PID=""; BRIDGE_PID=""; WORKLOAD_PID=""; PGBENCH_PID=""; RC=1

cleanup() {
    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10 INT
    stop_pid "$TRACER_PID" 15
    echo "probe: daemon log tail:"; tail -n 20 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "probe: bridge log tail:"; tail -n 20 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    sleep 1
    local leftover; leftover="$(pgrep -f "$TRACE_DIR" 2>/dev/null || true)"
    rm -rf "$TRACE_DIR"
    rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"
    if [[ -n "$leftover" ]]; then
        echo "ERROR: processes from this run survived teardown: $leftover"
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check_pgbench_provisioned "timeline_window_probe" "$PGBENCH_LOG"
echo "probe: starting pgbench (4 clients, ${DURATION_S}s, throttled)"
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" --rate=25 \
    >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!
sleep 1
kill -0 "$PGBENCH_PID" 2>/dev/null || { echo "ERROR: pgbench died"; tail -n 20 "$PGBENCH_LOG"; exit 1; }

echo "probe: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2
kill -0 "$WORKLOAD_PID" 2>/dev/null || { echo "ERROR: workload died"; tail -n 40 "$WORKLOAD_LOG"; exit 1; }

echo "probe: starting daemon (--mode full, $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" --mode full -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!
for _ in $(seq 1 60); do
    [[ -S "$SOCK" ]] && break
    kill -0 "$TRACER_PID" 2>/dev/null || { echo "ERROR: daemon exited"; tail -n 40 "$DAEMON_LOG"; exit 1; }
    sleep 0.5
done
[[ -S "$SOCK" ]] || { echo "ERROR: no control socket"; tail -n 40 "$DAEMON_LOG"; exit 1; }

echo "probe: starting bridge on :$PORT"
"$BRIDGE" --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER" \
    root@localhost >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!
BASE_URL="http://localhost:$PORT/"
for _ in $(seq 1 60); do
    url_ready "${BASE_URL}session" && break
    kill -0 "$BRIDGE_PID" 2>/dev/null || { echo "ERROR: bridge exited"; tail -n 40 "$BRIDGE_LOG"; exit 1; }
    sleep 0.5
done
url_ready "${BASE_URL}session" || { echo "ERROR: bridge never answered"; tail -n 40 "$BRIDGE_LOG"; exit 1; }
echo "probe: bridge ready at $BASE_URL"

# Let the daemon accumulate a couple of flush intervals of real data before
# the walk drills into a session -- an empty sessions table would refuse.
sleep 20

RC=0
# Demo-length ledger (issue #205 residual / #197): one row per live tick for
# PGWT_PROBE_LEDGER_MIN minutes, written to tests/results/timeline_ledger/
# summary.json. Runs INSTEAD of the short runs when set.
if [[ -n "${PGWT_PROBE_LEDGER_MIN:-}" ]]; then
    python3 "$SCRIPT_DIR/timeline_window_probe.py" --url "$BASE_URL" \
        --ledger "$PGWT_PROBE_LEDGER_MIN" \
        --out "$PROJECT_DIR/tests/results/timeline_ledger"
    exit $?
fi

for i in $(seq 1 "$RUNS"); do
    echo "=== probe run $i/$RUNS ==="
    python3 "$SCRIPT_DIR/timeline_window_probe.py" --url "$BASE_URL" \
        --ticks "$TICKS" --out "$PROJECT_DIR/tests/results/timeline_probe/run$i"
    rc=$?
    [[ $rc -ne 0 ]] && RC=$rc
done

if [[ "${PGWT_PROBE_EMULATE:-0}" != "0" ]]; then
    for i in $(seq 1 "$RUNS"); do
        echo "=== smoke-shaped emulation run $i/$RUNS ==="
        python3 "$SCRIPT_DIR/timeline_window_probe.py" --url "$BASE_URL" \
            --emulate-smoke --ticks 6 \
            --out "$PROJECT_DIR/tests/results/timeline_probe/emulate$i"
        rc=$?
        [[ $rc -ne 0 ]] && RC=$rc
    done
fi
exit "$RC"
