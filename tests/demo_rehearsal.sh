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
# Run via `make demo-rehearsal` (scripts/demo-rehearsal.sh), which either
# creates a throwaway Hetzner VM (the default, private to this one run) or
# -- when PGWT_BOX is set -- targets a PERSISTENT, shared box instead
# (owner rule 2026-09-28: persistent boxes first; required for a run that
# is meant to count toward docs/DEMO_REHEARSAL_CRITERIA.md's sequence).
# Not wired into tests/run_all.sh and never should be -- it is 30-45x
# longer than everything else run_all.sh does.
#
# No flock in THIS script (unlike tests/ui_live_smoke.sh) -- the lock now
# lives one level up, in scripts/demo-rehearsal-remote-run.sh (which wraps
# THIS whole script, `sudo`+build included, in
# /tmp/pgwt-box-check.lock -- the same lock box-check.sh uses), because
# that is the process that is actually detached and long-lived, and taking
# it there means an uncontended throwaway-VM run pays nothing extra while
# a PERSISTENT-box run correctly queues behind CI/other agents instead of
# colliding with them. remote-run.sh's `sudo ... tests/demo_rehearsal.sh`
# invocation (which is what actually starts everything below: tracer,
# bridge, pgwt-server, pgbench, the lock/sleep workload) is deliberately
# run with `200>&-` so none of THIS script's own children ever inherit
# that fd -- see the remote-run.sh comment for why an inherited fd on a
# SHARED persistent box is now a real hazard, not just a hypothetical one:
# cleanup() below already detects a leaked child and exits 1 but does NOT
# kill it, exactly like tests/ui_live_smoke.sh's own cleanup() documents.
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
    # item 4). On a throwaway VM a leftover process here still gets swept
    # away by that VM's own deletion right after this script exits -- but
    # on a PERSISTENT box (PGWT_BOX) nothing ever deletes it, so a leftover
    # process is no longer just an untidy-teardown signal, it is a
    # potentially permanent resident. Reported loudly (exit 1) either way;
    # this guard does not kill it, which is exactly why remote-run.sh's
    # `200>&-` matters -- the flock fd must not be among what survives.
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

# criteria doc (docs/DEMO_REHEARSAL_CRITERIA.md) §7: "no test anywhere in
# the run exited 126 (not executable) or 127 (command not found)" -- that
# is a broken invocation, never a known product bug. The kill -0 checks
# below already catch ANY early death regardless of its exit code; this
# just reaps the already-dead PID (harmless -- `wait` on a dead PID returns
# immediately) and names 126/127 specifically so that failure mode is never
# mistaken for a product regression in the log a human reads afterward.
report_early_exit() {
    local label="$1" pid="$2"
    local rc=0
    wait "$pid" 2>/dev/null; rc=$?
    if [[ "$rc" -eq 126 || "$rc" -eq 127 ]]; then
        echo "demo_rehearsal: $label exited $rc -- BROKEN INVOCATION (not executable / command not found), not a product bug"
    else
        echo "demo_rehearsal: $label exited with code $rc"
    fi
}

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
    report_early_exit "pgbench" "$PGBENCH_PID"
    tail -n 20 "$PGBENCH_LOG"
    exit 1
fi

echo "demo_rehearsal: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2
if ! kill -0 "$WORKLOAD_PID" 2>/dev/null; then
    echo "ERROR: lock/sleep workload exited immediately after starting:"
    report_early_exit "lock/sleep workload" "$WORKLOAD_PID"
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
        report_early_exit "daemon" "$TRACER_PID"
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
        report_early_exit "bridge" "$BRIDGE_PID"
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
python3 -u "$SCRIPT_DIR/demo_rehearsal.py" --url "$BASE_URL" \
    --trace-dir "$TRACE_DIR" --daemon-log "$DAEMON_LOG" \
    --duration-min "$DURATION_MIN" \
    --pgbench-pid "$PGBENCH_PID" --workload-pid "$WORKLOAD_PID" \
    --daemon-pid "$TRACER_PID"
DEMO_RC=$?

exit "$DEMO_RC"
