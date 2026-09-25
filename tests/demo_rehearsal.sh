#!/bin/bash
# demo_rehearsal.sh -- issue #157: "demo-rehearsal" harness. A single, long
# (default 35 min, DURATION_MIN=) --mode full capture against a real
# PostgreSQL under continuous pgbench + lock/sleep load, walked repeatedly
# (not once) across the whole window, plus three end-of-capture checks
# (time-model conservation, the Waterfall executions query's latency, a
# clean daemon log). This is the instrument that decides whether the owner
# can demo -- see docs/ROADMAP_AND_STATUS.md and CLAUDE.md.
#
# EXTENDS, does not fork, tests/ui_live_smoke.sh (CLAUDE.md): the daemon /
# bridge / pgbench / lock-sleep-workload setup below is the same sequence,
# sharing tests/live_daemon_lib.sh (PGPORT derivation, process teardown,
# readiness polling) and tests/live_loop_workload.py (the looping lock/
# sleep workload) so the two scripts cannot drift apart. What's genuinely
# different is the walk itself (tests/demo_rehearsal.py repeats the full
# 11-tab walk at early/middle/late offsets instead of once) and the extra
# end-of-capture checks it runs -- see that file's own header.
#
# Run via `make demo-rehearsal` (scripts/demo-rehearsal.sh), which creates
# the throwaway Hetzner VM this script is meant to run on (EPHEMERAL=1
# path, reusing tests/hetzner-vm.sh -- issue #157: "Never the persistent
# gate box, never a gating CI job"). Not wired into tests/run_all.sh and
# never should be -- it is 30-45x longer than everything else run_all.sh
# does.
#
# No flock here (unlike tests/ui_live_smoke.sh): this script is designed to
# run on a PRIVATE ephemeral VM that scripts/demo-rehearsal.sh creates for
# exactly one run and then deletes -- nothing else is ever sharing it, so
# the box-wide lock tests/ui_live_smoke.sh needs on the persistent gate box
# does not apply here. A manual run on a SHARED box must add it.
#
# Usage: tests/demo_rehearsal.sh [--pid POSTMASTER_PID] [--pg-version N]
# Env: DURATION_MIN (default 35), PGWT_DEMO_PORT (default 8385)
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
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_MAJOR="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_MAJOR" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_MAJOR")
    else
        PM_PID=$(find_postmaster)
    fi
fi
if [[ -z "$PM_PID" ]]; then
    echo "ERROR: cannot find postmaster PID"
    exit 1
fi
echo "demo_rehearsal: postmaster PID $PM_PID"

derive_pgport "$PM_PID" "$PG_MAJOR" "demo_rehearsal"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not built (make / make pgwt-client)"
        exit 1
    fi
done

# Default 35 min: the issue's documented range is 30-45 -- demo length, not
# smoke length (tests/ui_live_smoke.sh's own 30 min is already this long;
# this harness additionally REPEATS the full 11-tab walk 3x across the
# window instead of once, see tests/demo_rehearsal.py). Override only for
# the issue's explicit DURATION_MIN=3 self-test -- never for a real
# baseline run.
DURATION_MIN="${DURATION_MIN:-35}"
DURATION_S=$(python3 -c "print(int(float(\"$DURATION_MIN\") * 60))")
PORT="${PGWT_DEMO_PORT:-8385}"

TRACE_DIR=$(mktemp -d /tmp/pgwt_demo_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_demo_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_demo_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_demo_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_demo_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"

TRACER_PID=""
BRIDGE_PID=""
WORKLOAD_PID=""
PGBENCH_PID=""
DEMO_RC=1

# stop_pid: bounded wait (signal, poll, KILL) -- live_daemon_lib.sh. The
# bridge gets SIGINT specifically (web/main.go only handles os.Interrupt).
cleanup() {
    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10 INT
    stop_pid "$TRACER_PID" 15

    echo "demo_rehearsal: daemon log tail:"
    tail -n 60 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "demo_rehearsal: bridge log tail:"
    tail -n 40 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    echo "demo_rehearsal: workload log tail:"
    tail -n 20 "$WORKLOAD_LOG" 2>/dev/null | sed 's/^/  /'
    tail -n 20 "$PGBENCH_LOG" 2>/dev/null | sed 's/^/  /'

    # Same leaked-process guard as tests/ui_live_smoke.sh (issue #93 review
    # item 4) -- an ephemeral VM gets deleted right after this script exits
    # either way, but a leftover process here would still mean the run's
    # own teardown was not clean, worth reporting loudly.
    sleep 1
    local leftover=""
    leftover="$(pgrep -f "$TRACE_DIR" 2>/dev/null || true)"
    local pid
    for pid in "$PGBENCH_PID" "$WORKLOAD_PID" "$BRIDGE_PID" "$TRACER_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            leftover="$leftover"$'\n'"$pid"
        fi
    done
    leftover="$(echo "$leftover" | sed '/^$/d')"

    rm -rf "$TRACE_DIR"
    rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"

    if [[ -n "$leftover" ]]; then
        echo "ERROR: processes from this run survived teardown:"
        echo "$leftover" | while read -r p; do
            [[ -n "$p" ]] && ps -o pid,ppid,cmd -p "$p" 2>/dev/null
        done
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ── 1. Controlled load ───────────────────────────────────────────────────────
check_pgbench_provisioned "demo_rehearsal" "$PGBENCH_LOG"

echo "demo_rehearsal: starting pgbench (4 clients, ${DURATION_S}s, throttled)"
# --rate: same 25 tx/s reasoning as tests/ui_live_smoke.sh (an unthrottled
# --mode full capture over the WHOLE window grows the executions dataset
# enough to make the end-of-capture Waterfall-latency check meaningless --
# it would be measuring "how slow does an unrealistically large dataset
# get", not the realistic-load regression issue #101 describes).
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" --rate=25 \
    >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!
sleep 1
if ! kill -0 "$PGBENCH_PID" 2>/dev/null; then
    echo "ERROR: pgbench exited immediately after starting:"
    tail -n 20 "$PGBENCH_LOG"
    exit 1
fi

echo "demo_rehearsal: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2
if ! kill -0 "$WORKLOAD_PID" 2>/dev/null; then
    echo "ERROR: lock/sleep workload exited immediately after starting:"
    tail -n 40 "$WORKLOAD_LOG"
    exit 1
fi

# ── 2. Daemon (--mode full -- see tests/ui_live_smoke.sh's DEVIATION note
# for why full, not tiered: plan/execute USDT probes, which Waterfall's
# executions query needs, are gated on --mode full only). --daemon: long-
# running, no fixed capture budget -- DURATION_S below decides how long.
echo "demo_rehearsal: starting daemon (--mode full, trace dir $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode full -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 60); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "ERROR: daemon exited during startup:"
        tail -n 40 "$DAEMON_LOG"
        exit 1
    fi
    sleep 0.5
done
if [[ ! -S "$SOCK" ]]; then
    echo "ERROR: control socket never appeared at $SOCK"
    tail -n 40 "$DAEMON_LOG"
    exit 1
fi
echo "demo_rehearsal: control socket ready"

# ── 3. Go bridge ─────────────────────────────────────────────────────────────
echo "demo_rehearsal: starting Go bridge on :$PORT against root@localhost"
"$BRIDGE" --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER" \
    root@localhost >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!

BASE_URL="http://localhost:$PORT/"
for _ in $(seq 1 60); do
    if url_ready "${BASE_URL}session"; then
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        echo "ERROR: bridge exited during startup:"
        tail -n 40 "$BRIDGE_LOG"
        exit 1
    fi
    sleep 0.5
done
if ! url_ready "${BASE_URL}session"; then
    echo "ERROR: bridge never answered ${BASE_URL}session"
    tail -n 40 "$BRIDGE_LOG"
    exit 1
fi
echo "demo_rehearsal: bridge ready at $BASE_URL"

# ── 4. The rehearsal: repeated walk + end-of-capture checks ─────────────────
# Daemon and bridge stay UP through this whole call (including the
# end-of-capture pgwt-server queries tests/demo_rehearsal.py issues
# directly against $TRACE_DIR) -- teardown only happens in cleanup() above,
# after this returns, same ordering as tests/ui_live_smoke.sh.
python3 "$SCRIPT_DIR/demo_rehearsal.py" --url "$BASE_URL" \
    --trace-dir "$TRACE_DIR" --daemon-log "$DAEMON_LOG" \
    --duration-min "$DURATION_MIN" \
    --pgbench-pid "$PGBENCH_PID" --workload-pid "$WORKLOAD_PID"
DEMO_RC=$?

exit "$DEMO_RC"
