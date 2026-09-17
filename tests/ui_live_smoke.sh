#!/bin/bash
# ui_live_smoke.sh — issue #93 phase 2: runs tests/ui_live_smoke.py against a
# REAL daemon + REAL Go bridge (not tests/mock_server.py). This is the ONLY
# front-end for tests/ui_live_smoke.py that a human is expected to run
# directly; it is wired into tests/run_all.sh's root+PG live section, so
# `sudo tests/run_all.sh --require-live` (== `make box-check`) runs it too.
#
# What it does, in order (mirrors the proven start/stop patterns in
# tests/test_capture_smoke.py and tests/test_escalation.sh):
#   1. Controlled load: pgbench (4 clients, background, for the whole run) +
#      a LOOPING lock/sleep workload (tests/test_capture_smoke.py's Workload
#      class, fired repeatedly instead of once) so Lock:relation and
#      Timeout:PgSleep keep showing up in every live tick, not just the
#      first one.
#   2. pg_wait_tracer --daemon --mode full on a fresh temp trace dir.
#   3. The Go bridge (web/pgwt) against root@localhost, pointed at the
#      daemon's trace dir.
#   4. tests/ui_live_smoke.py --url against the bridge's local URL.
#   5. Reverse-order teardown, propagating ui_live_smoke.py's exit code
#      (verified: `exit N` inside a function whose EXIT trap's last command
#      fails/succeeds still leaves the process exit code at N — bash keeps
#      the code from the `exit` call that triggered the trap).
#
# DEVIATION from the issue's stated design ("Daemon in --mode tiered; trigger
# one escalation ... so the exact-tier panels (Transitions, Waterfall,
# Scatter, Matrix) have data"): measured on the gate box (2026-09-16),
# src/daemon.c gates the query__execute__start/done and query__plan__*
# USDT probes on `d->mode == PGWT_MODE_FULL` UNCONDITIONALLY -- an escalated
# window under --mode tiered never attaches them (the gate is checked once
# at daemon startup, not per-window). Executions/Waterfall/Scatter need
# those markers (src/server.c handle_executions pairs EXEC_START/EXEC_END);
# under tiered+escalate they showed "No executions for selected range" for
# the whole run, confirmed by reading src/daemon.c's own comment ("Plan/
# execute USDT probes remain full-mode-only, as before Stage 3"). Transitions
# and Matrix do NOT need these (they read the plain wait-event stream) and
# rendered correctly under tiered+escalate. --mode full has no escalation
# concept (always full fidelity; pgwt_escalate() would return "escalation
# requires --mode tiered"), so step 3's control-socket escalate is REMOVED,
# not merely skipped. Flagged as a Design question for the owner: whether to
# formally change the issue's design decision, or accept "no exact-tier data,
# ever" as the correct tiered-mode UI state for these two tabs and lower the
# bar for them specifically. Full mode is what makes today's evidence run
# actually exercise all 11 tabs with real data.
#
# RATIFIED (2026-09-17, lead): --mode full stays -- this test's job is to
# exercise every panel, and full is the only mode that feeds them all. The
# tiered+escalation USDT question above is being raised with the owner
# separately as its own decision, independent of this test.
#
# Requirements (gate box, tests/provision-runner.sh):
#   - root, a running PostgreSQL with pg_stat_statements preloaded.
#   - Playwright + Chromium installed for the box's python3 (same package
#     tests/run_all.sh's web-UI section already requires).
#   - root's OWN ssh public key present in root's authorized_keys, AND
#     localhost/127.0.0.1 already in root's known_hosts (web/bridge.go's
#     NewSSHBridge runs ssh with `-o BatchMode=yes`, which REFUSES rather
#     than prompts on an unknown host key or missing key auth -- a silent
#     hang here shows up as "bridge never answered /session" below with an
#     unhelpful log). Neither is set up by this script — provisioning's job.
#   - pg_wait_tracer, pgwt-server, web/pgwt already built (`make` /
#     `make pgwt-client`) — tests/run_all.sh's Step 0 does this.
#
# This script cannot be run on the Mac (no root, no PG, no BPF, no Linux) —
# it is exercised by `bash -n` only until it runs on the box in phase 2.
#
# LOCK DISCIPLINE (the gate box is shared): every CPU-using step this script
# runs -- make, Chromium/Playwright, ffmpeg video muxing, pgbench, the
# daemon -- MUST run inside `flock /tmp/pgwt-box-check.lock ...` on the box,
# exactly like scripts/box-check.sh's own `sudo tests/run_all.sh` call. This
# script does not grab the lock itself (run_all.sh's caller does, once, for
# the whole live section); a one-off manual invocation on the box (as in
# this file's own header example) MUST be wrapped the same way, e.g.:
#   flock /tmp/pgwt-box-check.lock sudo tests/ui_live_smoke.sh --pg-version 17
# A concurrent unlocked run caused transient test_cli misses for another
# agent sharing this box (2026-09-17) -- never run this, or any manual
# daemon/pgbench/bridge session, outside the lock.
#
# Usage: sudo tests/ui_live_smoke.sh [--pid POSTMASTER_PID] [--pg-version N]
#
# --pg-version N: on a multi-cluster box (the gate box runs PG 13/16/17/18
# at once, one postgres process each) this picks the postmaster via
# testutil.sh's find_postmaster --pg-version AND exports PGPORT=5400+N so
# every psql/pgbench call below (Debian's pg_wrapper reads PGPORT, and it
# survives sudo) targets that exact cluster instead of whichever "postgres"
# pgrep happens to see first. Not currently passed down by run_all.sh's
# LIVE_TESTS (which only forwards --pid) -- pass it directly when running
# this script by hand on a multi-cluster box.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

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

if [[ -n "$PG_MAJOR" ]]; then
    export PGPORT=$((5400 + PG_MAJOR))
    echo "ui_live_smoke: PG major $PG_MAJOR -> PGPORT=$PGPORT"
fi

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
echo "ui_live_smoke: postmaster PID $PM_PID"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not built (make / make pgwt-client)"
        exit 1
    fi
done

# 30 min: measured on the gate box (2026-09-16/17), the real walk (real
# network + a real --mode full daemon, vs. the Mac --mock walk's ~3-4 min)
# took ~20-25 min end to end -- pgbench and the lock/sleep workload must
# outlast the WHOLE walk, not just its first minute, or the last few tabs
# (Waterfall, Scatter, Matrix) see a live window with no recent activity at
# all and legitimately render empty. Override for local iteration only --
# never lower this in a gating run.
DURATION_S="${PGWT_UI_LIVE_DURATION_S:-1800}"
PORT="${PGWT_UI_LIVE_PORT:-8384}"

TRACE_DIR=$(mktemp -d /tmp/pgwt_ui_live_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_ui_live_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_ui_live_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_ui_live_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_ui_live_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"

TRACER_PID=""
BRIDGE_PID=""
WORKLOAD_PID=""
PGBENCH_PID=""
SMOKE_RC=1

# Bounded wait: TERM, then poll for exit, then KILL if it outlives the
# budget. Mirrors test_capture_smoke.py's terminate_and_wait (TERM + timeout
# + KILL) -- `wait` has no native timeout in bash.
stop_pid() {
    local pid="$1" budget_s="${2:-10}"
    [[ -z "$pid" ]] && return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill -TERM "$pid" 2>/dev/null
    local steps=$((budget_s * 2))
    local n=0
    while kill -0 "$pid" 2>/dev/null && [[ $n -lt $steps ]]; do
        sleep 0.5
        n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
    return 0
}

cleanup() {
    # Reverse start order.
    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10
    stop_pid "$TRACER_PID" 15
    echo "ui_live_smoke: daemon log tail:"
    tail -n 40 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "ui_live_smoke: bridge log tail:"
    tail -n 40 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    echo "ui_live_smoke: workload log tail:"
    tail -n 20 "$WORKLOAD_LOG" 2>/dev/null | sed 's/^/  /'
    tail -n 20 "$PGBENCH_LOG" 2>/dev/null | sed 's/^/  /'
    rm -rf "$TRACE_DIR"
    rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"
}
trap cleanup EXIT

# GET a URL, exit 0 iff it answers 200. Python (not curl) to avoid adding a
# new external-tool dependency on top of what the rest of the suite needs.
url_ready() {
    python3 - "$1" <<'PYEOF'
import sys, urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
        sys.exit(0 if r.status == 200 else 1)
except Exception:
    sys.exit(1)
PYEOF
}

# ── 1. Controlled load ───────────────────────────────────────────────────────

echo "ui_live_smoke: initializing pgbench schema"
pgbench -U postgres -d postgres -i -s 1 >>"$PGBENCH_LOG" 2>&1

echo "ui_live_smoke: starting pgbench (4 clients, ${DURATION_S}s, throttled)"
# --rate: measured on the gate box, an UNTHROTTLED 4-client pgbench against
# --mode full (every event captured, not sampled) for the full DURATION_S
# grows the executions dataset large enough that Waterfall's "latest
# execution" query stopped answering within the issue's 60s no-data budget
# by the time the walk reached it (~15-20 min in). 25 tx/s keeps Lock/
# Timeout/CPU/IO wait classes and a growing executions list genuinely
# present (the issue's actual requirement) without the volume a smoke test
# has no need to generate. Also lighter on shared box CPU -- see the lock-
# discipline note above the file.
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" --rate=25 \
    >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!

# Lock/Timeout: reuse tests/test_capture_smoke.py's Workload class (the same
# holder/waiter/sleeper psql sessions that test already proves out) but LOOP
# fire()/release() for the whole run instead of firing once, so Lock:relation
# and Timeout:PgSleep keep appearing in every live tick.
echo "ui_live_smoke: starting looping lock/sleep workload"
python3 - "$SCRIPT_DIR" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 <<'PYEOF' &
import signal
import sys
import time

sys.path.insert(0, sys.argv[1])
from test_capture_smoke import Workload

duration_s = float(sys.argv[2])
stop = {"flag": False}


def _stop(signum, frame):
    stop["flag"] = True


signal.signal(signal.SIGTERM, _stop)

wl = Workload()
wl.open_sessions()
deadline = time.monotonic() + duration_s
try:
    while not stop["flag"] and time.monotonic() < deadline:
        # Workload.release() COMMITs the holder, permanently dropping the
        # lock it took in open_sessions() -- a second fire() without
        # re-acquiring it would have the waiter sail through with no
        # Lock:relation wait at all. Re-issue the same BEGIN/LOCK
        # open_sessions() used, then fire()/release() as normal.
        wl.holder.stdin.write(
            f"BEGIN; LOCK TABLE {wl.LOCK_TABLE} IN ACCESS EXCLUSIVE MODE;\n")
        wl.holder.stdin.flush()
        time.sleep(0.5)   # let the re-lock land before the waiter tries
        wl.fire(sleep_s=3)
        time.sleep(2)
        wl.release()
        time.sleep(1)
finally:
    wl.stop()
PYEOF
WORKLOAD_PID=$!

# ── 2. Daemon (--mode full: see the DEVIATION note above) ───────────────────
# --daemon: long-running (reconnect on PG restart), no --count/--duration
# bound -- the UI walk's own length decides how long we need it, not a fixed
# capture budget (test_escalation.sh's short-lived --duration daemon is the
# wrong shape here). --mode full is always full-fidelity from the first
# tick -- no escalate call, no budget, no warm-up window needed.
echo "ui_live_smoke: starting daemon (--mode full, trace dir $TRACE_DIR)"
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
echo "ui_live_smoke: control socket ready"

# ── 3. Go bridge ─────────────────────────────────────────────────────────────
echo "ui_live_smoke: starting Go bridge on :$PORT against root@localhost"
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
echo "ui_live_smoke: bridge ready at $BASE_URL"

# ── 4. The walk ───────────────────────────────────────────────────────────────
python3 "$SCRIPT_DIR/ui_live_smoke.py" --url "$BASE_URL"
SMOKE_RC=$?

exit "$SMOKE_RC"
