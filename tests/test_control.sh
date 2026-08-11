#!/bin/bash
# test_control.sh — Integration test for the daemon control socket (Phase A0)
#
# Starts the daemon against a real PostgreSQL with trace recording,
# then exercises {trace_dir}/pgwt.sock:
#   - status / metrics JSON shape
#   - unknown command + invalid JSON errors
#   - concurrent clients
#   - client disconnect mid-request (daemon must survive)
#   - pgwt-server "control" proxy command
#   - pgwt-server --dump daemon status block
#   - clean shutdown (socket unlinked)
#
# Requires: root, running PostgreSQL, python3.
# Usage: sudo tests/test_control.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
SERVER="$SCRIPT_DIR/../pgwt-server"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "ERROR: cannot find postmaster PID"
        exit 1
    fi
fi

echo "=== test_control ==="
echo "Postmaster PID: $PM_PID"

TRACE_DIR=$(mktemp -d /tmp/pgwt_control_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_control_daemon_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""

passed=0
failed=0

check() {
    if [[ "$1" -eq 0 ]]; then
        echo "  PASS: $2"
        passed=$((passed + 1))
    else
        echo "  FAIL: $2"
        failed=$((failed + 1))
    fi
}

cleanup() {
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null
        wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$TRACE_DIR" "$DAEMON_LOG"
}
trap cleanup EXIT

# Send one JSON line to the socket, print the one-line response.
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

jget() { python3 -c "import json,sys; print(json.load(sys.stdin)$1)"; }

# ── Start daemon ──────────────────────────────────────────────
# Deliberately NO --mode: this also serves as the "default mode is tiered"
# check. Started bare, the daemon must come up in tiered mode with the
# always-on sampler and NO watchpoints (tier=sampled) until an escalation.
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" -v \
    --anomaly-cpu-capacity 3.25 \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 30); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "ERROR: daemon exited during startup:"
        tail -20 "$DAEMON_LOG"
        exit 1
    fi
    sleep 0.5
done

[[ -S "$SOCK" ]]
check $? "control socket created at \$TRACE_DIR/pgwt.sock"
if [[ ! -S "$SOCK" ]]; then
    tail -20 "$DAEMON_LOG"
    exit 1
fi

MODE=$(stat -c %a "$SOCK")
[[ "$MODE" == "600" ]]
check $? "socket mode is 0600 (got $MODE)"

# ── status ────────────────────────────────────────────────────
STATUS=$(ctl '{"cmd":"status"}')
echo "  status: $STATUS"
echo "$STATUS" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['ok'] is True, r
# Default mode is now tiered (low-overhead always-on sampler).
assert r['mode'] == 'tiered', r
# Tiered starts de-escalated: the sampler is the active tier, no watchpoints.
assert r['tier'] == 'sampled', r
assert r['escalation_supported'] is True, r
assert isinstance(r['uptime_s'], (int, float)) and r['uptime_s'] >= 0, r
assert isinstance(r['backends'], int) and r['backends'] >= 0, r
assert r['pg_pid'] == $PM_PID, r
assert isinstance(r['version'], str) and r['version'], r
# T4/SMP-1: sampler health must be present and healthy on a working box
assert r['sampler_healthy'] is True, r
assert r['sampler_unhealthy_reason'] == '', r
# T8 (§5.4): measured-CPU capability must be reported, never silent.
assert r['cpu_accounting'] in ('measured', 'legacy'), r
"
check $? "default mode is tiered, tier=sampled (no watchpoints until escalation)"

# ── metrics (initial) ─────────────────────────────────────────
METRICS=$(ctl '{"cmd":"metrics"}')
echo "  metrics: $METRICS"
echo "$METRICS" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['ok'] is True, r
for k in ('events_total', 'events_per_sec', 'lifecycle_events_total',
          'wp_attach_failures_total', 'backends_tracked',
          'trace_events_written_total', 'trace_bytes_written_total',
          'uptime_s',
          # T4 capture-hardening counters (CAP-1/2/5/6, SMP-1/3)
          'state_map_full_total', 'seen_query_ids_full_total',
          'invalid_wait_reads_total', 'sampler_ticks_missed_total',
          'state_reseeds_total',
          # AAS-1 Stage 3 CPU-saturation rule counter/state.
          'anomaly_cpu_saturation_fires_total', 'anomaly_cpu_cusum',
          # T8 measured-CPU counters (§5.6). Present + numeric here; the exact
          # magnitudes are proven by the pure-CPU straddle acceptance test.
          'cpu_ns_total', 'offcpu_ns_total', 'cpu_clamped_total',
          'wait_gap_cpu_ns_total'):
    assert k in r, 'missing ' + k
    assert isinstance(r[k], (int, float)), k
# T8: capability string mirrors status.
assert r['cpu_accounting'] in ('measured', 'legacy'), r
# AAS-1 Stage 2: the explicit capacity override wins and is observable.
assert r['effective_cpu_capacity_cores'] == 3.25, r
assert r['effective_cpu_capacity_source'] == 'override', r
assert r['anomaly_cpu_cusum_enabled'] is True, r
"
check $? "metrics: all counters present and numeric"

# ── ESC-9: metrics.tier must agree with status.tier (never derive tier
#    solely from escalation.active — that reported "sampled" in --mode full). ─
STATUS_TIER=$(echo "$STATUS" | jget "['tier']")
METRICS_TIER=$(echo "$METRICS" | jget "['tier']")
[[ "$STATUS_TIER" == "$METRICS_TIER" ]]
check $? "metrics.tier == status.tier (ESC-9; both '$STATUS_TIER')"

# Tiered default: the always-on tier is the sampler, so it's samples_total
# (not the watchpoint events_total) that moves. samples_total is part of the
# sampled-tier metrics block.
SAMPLES_BEFORE=$(echo "$METRICS" | python3 -c \
    "import json,sys; print(json.load(sys.stdin).get('samples_total', 0))")

# ── generate load, counters must move ─────────────────────────
for i in 1 2 3; do
    psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM generate_series(1,200000); SELECT pg_sleep(0.2);" \
        >/dev/null 2>&1
done
sleep 2   # let several sampler ticks accumulate

METRICS2=$(ctl '{"cmd":"metrics"}')
SAMPLES_AFTER=$(echo "$METRICS2" | python3 -c \
    "import json,sys; print(json.load(sys.stdin).get('samples_total', 0))")
[[ "$SAMPLES_AFTER" -gt "$SAMPLES_BEFORE" ]]
check $? "samples_total increased under load ($SAMPLES_BEFORE -> $SAMPLES_AFTER)"

BYTES=$(echo "$METRICS2" | python3 -c \
    "import json,sys; print(json.load(sys.stdin)['trace_bytes_written_total'])")
[[ "$BYTES" -gt 0 ]]
check $? "trace_bytes_written_total > 0 ($BYTES)"

# ── unknown command ───────────────────────────────────────────
RESP=$(ctl '{"cmd":"bogus"}')
[[ "$RESP" == '{"ok":false,"error":"unknown command"}' ]]
check $? "unknown command rejected (got: $RESP)"

# ── invalid JSON ──────────────────────────────────────────────
RESP=$(ctl 'this is not json')
echo "$RESP" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['ok'] is False and 'error' in r, r
"
check $? "invalid JSON rejected (got: $RESP)"

# ── concurrent clients + disconnect mid-request ───────────────
python3 - "$SOCK" <<'PYEOF'
import json, socket, sys

path = sys.argv[1]

def conn():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(path)
    return s

def ask(s, req):
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("daemon closed connection")
        buf += chunk
    return json.loads(buf.decode().split("\n")[0])

# Two clients interleaved on open connections
a, b = conn(), conn()
ra = ask(a, {"cmd": "status"})
rb = ask(b, {"cmd": "metrics"})
ra2 = ask(a, {"cmd": "metrics"})   # second request on same connection
assert ra["ok"] and rb["ok"] and ra2["ok"]
b.close()

# Disconnect mid-request: partial line, no newline, then close
c = conn()
c.sendall(b'{"cmd":"sta')
c.close()

# Daemon must still answer on the surviving and on a fresh connection
ra3 = ask(a, {"cmd": "status"})
assert ra3["ok"]
a.close()
d = conn()
rd = ask(d, {"cmd": "status"})
assert rd["ok"]
d.close()
PYEOF
check $? "concurrent clients + disconnect mid-request survived"

kill -0 "$TRACER_PID" 2>/dev/null
check $? "daemon still alive after client abuse"

# ── ESC-10: a second daemon must REFUSE to steal a live control socket ─────
# The old code unconditionally unlink()'d the socket, letting a second daemon
# hijack a running daemon's control plane (and its trace dir). The liveness
# probe must make the second daemon exit non-zero with a clear message while
# the original daemon and its socket survive.
SECOND_LOG=$(mktemp /tmp/pgwt_control_second_XXXXXX.log)
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" -v \
    >/dev/null 2>"$SECOND_LOG"
SECOND_RC=$?
[[ $SECOND_RC -ne 0 ]] && grep -qi "already running" "$SECOND_LOG"
check $? "second daemon on the same trace dir refused (rc=$SECOND_RC, ESC-10)"
# Original daemon + socket must be untouched.
kill -0 "$TRACER_PID" 2>/dev/null && [[ -S "$SOCK" ]]
check $? "original daemon + live socket survive the refused second start"
ORIG_OK=$(ctl '{"cmd":"status"}' | jget "['ok']" 2>/dev/null)
[[ "$ORIG_OK" == "True" ]]
check $? "original daemon still answers on its control socket (ESC-10)"
rm -f "$SECOND_LOG"

# ── pgwt-server control proxy ─────────────────────────────────
RESP=$(echo '{"id":7,"cmd":"control","request":{"cmd":"status"}}' | \
       "$SERVER" "$TRACE_DIR" 2>/dev/null)
echo "  proxy: $RESP"
echo "$RESP" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['id'] == 7, r
assert r['response']['ok'] is True, r
assert r['response']['mode'] == 'tiered', r
"
check $? "pgwt-server control proxy forwards status"

RESP=$(echo '{"id":8,"cmd":"control"}' | "$SERVER" "$TRACE_DIR" 2>/dev/null)
echo "$RESP" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['id'] == 8 and 'error' in r, r
"
check $? "control proxy rejects missing request object"

# ── pgwt-server --dump status block ───────────────────────────
DUMP=$("$SERVER" --dump "$TRACE_DIR" 2>/dev/null | head -3)
echo "$DUMP" | grep -q "Daemon: running.*mode: tiered.*uptime"
check $? "--dump prints daemon status block"

# ── clean shutdown ────────────────────────────────────────────
kill -TERM "$TRACER_PID"
SHUT_OK=1
for _ in $(seq 1 20); do
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then SHUT_OK=0; break; fi
    sleep 0.5
done
wait "$TRACER_PID" 2>/dev/null
check $SHUT_OK "daemon exits on SIGTERM"
TRACER_PID=""

[[ ! -e "$SOCK" ]]
check $? "socket unlinked on shutdown"

# ── proxy with daemon stopped → clean error ───────────────────
RESP=$(echo '{"id":9,"cmd":"control","request":{"cmd":"status"}}' | \
       "$SERVER" "$TRACE_DIR" 2>/dev/null)
echo "$RESP" | python3 -c "
import json, sys
r = json.load(sys.stdin)
assert r['id'] == 9, r
assert r.get('error') == 'daemon not running', r
"
check $? "proxy reports 'daemon not running' when socket absent (got: $RESP)"

echo ""
echo "$passed/$((passed + failed)) tests passed"
[[ $failed -eq 0 ]]
