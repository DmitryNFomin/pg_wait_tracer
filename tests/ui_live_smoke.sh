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
#   2. pg_wait_tracer --daemon --mode tiered on a fresh temp trace dir.
#   3. ONE escalation via the control socket (unlimited daemon budget, a
#      window as long as the whole run) so the exact-tier panels
#      (Transitions, Waterfall, Scatter, Matrix) have data for the entire
#      walk, not just its first minute.
#   4. The Go bridge (web/pgwt) against root@localhost, pointed at the
#      daemon's trace dir.
#   5. tests/ui_live_smoke.py --url against the bridge's local URL.
#   6. Reverse-order teardown, propagating ui_live_smoke.py's exit code
#      (verified: `exit N` inside a function whose EXIT trap's last command
#      fails/succeeds still leaves the process exit code at N — bash keeps
#      the code from the `exit` call that triggered the trap).
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
# Usage: sudo tests/ui_live_smoke.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

TRACER="$PROJECT_DIR/pg_wait_tracer"
SERVER="$PROJECT_DIR/pgwt-server"
BRIDGE="$PROJECT_DIR/web/pgwt"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
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

# 10 min: generous over the observed Mac --mock walk (~3-4 min for all 11
# tabs at 6 ticks each; the box adds real network/render latency). Override
# for local iteration only -- never lower this in a gating run.
DURATION_S="${PGWT_UI_LIVE_DURATION_S:-600}"
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

# Send one JSON line to the control socket, print the one-line response.
# Same idiom as tests/test_escalation.sh's ctl().
ctl() {
    python3 - "$SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
s.sendall((sys.argv[2] + "\n").encode())
buf = b""
while b"\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

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

echo "ui_live_smoke: starting pgbench (4 clients, ${DURATION_S}s)"
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" \
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

# ── 2. Daemon (tiered, unlimited escalation budget) ─────────────────────────
# --daemon: long-running (reconnect on PG restart), no --count/--duration
# bound -- the UI walk's own length decides how long we need it, not a fixed
# capture budget (test_escalation.sh's short-lived --duration daemon is the
# wrong shape here). unlimited budget: a single long escalation below must
# never be denied by a budget mismatch with DURATION_S.
echo "ui_live_smoke: starting daemon (--mode tiered, trace dir $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode tiered --sample-rate 50 --escalation-budget unlimited -v \
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

# ── 3. One escalation, spanning the whole run ───────────────────────────────
ESC=$(ctl "{\"cmd\":\"escalate\",\"duration_s\":$DURATION_S,\"reason\":\"ui_live_smoke\"}")
echo "ui_live_smoke: escalate response: $ESC"
if ! echo "$ESC" | python3 -c "import json,sys; sys.exit(0 if json.load(sys.stdin).get('ok') else 1)"; then
    echo "ERROR: escalation was not granted"
    exit 1
fi

# ── 4. Go bridge ─────────────────────────────────────────────────────────────
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

# ── 5. The walk ───────────────────────────────────────────────────────────────
python3 "$SCRIPT_DIR/ui_live_smoke.py" --url "$BASE_URL"
SMOKE_RC=$?

exit "$SMOKE_RC"
