#!/bin/bash
# test_anomaly_live.sh — Live anomaly-triggered escalation test (Phase A5)
#
# Runs the daemon in --mode tiered, establishes a quiet baseline, then injects
# a LOCK TABLE storm (many sessions blocked on one AccessExclusive lock). The
# blocked backends sample as Lock-class, driving the lock-fraction (and AAS)
# anomaly rules over threshold. Asserts the full A5 lifecycle:
#
#   1. baseline: status tier=sampled, anomaly armed, fires_total == 0
#   2. lock storm -> daemon AUTO-escalates (tier=escalated, reason anomaly)
#   3. the window captures FULL transitions + an ANOMALY escalation marker
#      (dump_markers shows START reason=anomaly)
#   4. the bounded window auto-de-escalates back to sampled
#   5. cooldown: a second storm inside cooldown does NOT re-fire
#      (anomaly_dropped_cooldown_total rises; fires_total unchanged)
#
# Requires: root, running PostgreSQL, python3, psql in PATH.
# Usage: sudo tests/test_anomaly_live.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
DUMP="$SCRIPT_DIR/dump_markers"
source "$SCRIPT_DIR/testutil.sh"

PSQL=${PSQL:-psql}
PGUSER=${PGUSER:-postgres}
PGDB=${PGDB:-postgres}

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done
[[ -z "$PM_PID" ]] && PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then echo "ERROR: cannot find postmaster"; exit 1; fi

echo "=== test_anomaly_live ==="
echo "Postmaster PID: $PM_PID"

TRACE_DIR=$(mktemp -d /tmp/pgwt_anom_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_anom_daemon_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""
declare -a STORM_PIDS=()

# Budget large enough for several windows. The per-trigger window is short so
# it de-escalates within the test. The cooldown is long enough that a second
# storm injected right after the first window closes is still INSIDE cooldown
# (so it must NOT re-fire).
BUDGET=200
COOLDOWN=45
WINDOW=8

passed=0; failed=0
check() {
    if [[ "$1" -eq 0 ]]; then echo "  PASS: $2"; passed=$((passed+1));
    else echo "  FAIL: $2"; failed=$((failed+1)); fi
}

q() { $PSQL -U "$PGUSER" -d "$PGDB" -tAc "$1" 2>/dev/null; }

# Launch N sessions that each block trying to take an AccessExclusive lock on
# a table already locked by a holder. Returns once they are launched.
LOCK_TABLE="pgwt_anom_lock"
start_holder() {
    # Holder: take ACCESS EXCLUSIVE and sleep, keeping the lock for `$1` seconds.
    $PSQL -U "$PGUSER" -d "$PGDB" -c \
      "BEGIN; LOCK TABLE $LOCK_TABLE IN ACCESS EXCLUSIVE MODE; SELECT pg_sleep($1); COMMIT;" \
      >/dev/null 2>&1 &
    HOLDER_PID=$!
}
start_storm() {
    # $1 = number of blocked waiters, $2 = how long each tries (s)
    STORM_PIDS=()
    for _ in $(seq 1 "$1"); do
        $PSQL -U "$PGUSER" -d "$PGDB" -c \
          "BEGIN; SET lock_timeout='${2}s'; LOCK TABLE $LOCK_TABLE IN ACCESS EXCLUSIVE MODE; COMMIT;" \
          >/dev/null 2>&1 &
        STORM_PIDS+=($!)
    done
}
reap_storm() {
    for p in "${STORM_PIDS[@]:-}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
    STORM_PIDS=()
    [[ -n "${HOLDER_PID:-}" ]] && { wait "$HOLDER_PID" 2>/dev/null; HOLDER_PID=""; }
}

cleanup() {
    reap_storm
    q "DROP TABLE IF EXISTS $LOCK_TABLE;" >/dev/null 2>&1
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null; wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$TRACE_DIR" "$DAEMON_LOG"
}
trap cleanup EXIT

ctl() {
    python3 - "$SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf = b""
while b"\n" not in buf:
    c = s.recv(4096)
    if not c: break
    buf += c
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}
jget() { python3 -c "import json,sys; print(json.load(sys.stdin)$1)"; }

q "CREATE TABLE IF NOT EXISTS $LOCK_TABLE (x int);" >/dev/null 2>&1

# ── Start daemon in tiered mode with anomaly triggers tuned for the test ──
# High sample rate so the sustained-tick counter accrues fast; small sustain
# counts and a low lock-fraction so the deterministic storm trips it quickly.
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode tiered --sample-rate 50 --escalation-budget "$BUDGET" \
    --anomaly-aas-factor 3.0 --anomaly-aas-ticks 5 \
    --anomaly-lock-fraction 0.30 --anomaly-cooldown-s "$COOLDOWN" \
    --anomaly-window-s "$WINDOW" -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 30); do
    [[ -S "$SOCK" ]] && break
    kill -0 "$TRACER_PID" 2>/dev/null || { echo "daemon died:"; tail -20 "$DAEMON_LOG"; exit 1; }
    sleep 0.5
done
[[ -S "$SOCK" ]]; check $? "control socket created"
[[ -S "$SOCK" ]] || { tail -20 "$DAEMON_LOG"; exit 1; }

# ── 1. baseline: quiet, armed, no fires ───────────────────────
sleep 3   # let the baseline warm up on a quiet system
STATUS=$(ctl '{"cmd":"status"}'); echo "  baseline status: $STATUS"
echo "$STATUS" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['mode']=='tiered' and r['tier']=='sampled', r
assert r['escalation_supported'] is True, r
"
check $? "baseline: tiered/sampled, escalation supported"

FIRES0=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_fires_total']")
[[ "$FIRES0" == "0" ]]; check $? "no anomaly fires at baseline (got $FIRES0)"

# ── 2. inject the lock storm → expect AUTO-escalation ─────────
echo "  injecting lock storm..."
start_holder $((WINDOW + 6))     # hold the lock long enough to keep waiters blocked
sleep 0.5
start_storm 12 $((WINDOW + 4))   # 12 backends pile up on the Lock wait

# Poll status for up to ~8s for the auto-escalation.
ESCALATED=1
for _ in $(seq 1 40); do
    TIER=$(ctl '{"cmd":"status"}' | jget "['tier']" 2>/dev/null)
    if [[ "$TIER" == "escalated" ]]; then ESCALATED=0; break; fi
    sleep 0.2
done
check $ESCALATED "lock storm AUTO-escalated to full fidelity (tier=$TIER)"

FIRES1=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_fires_total']")
[[ "$FIRES1" -ge 1 ]]; check $? "anomaly_fires_total >= 1 (got $FIRES1)"

# daemon log should record the auto-escalation with a rule name
grep -q "anomaly AUTO-escalation" "$DAEMON_LOG"
check $? "daemon logged the anomaly auto-escalation"

# ── 3. window captures transitions + an ANOMALY marker ────────
# Let the window run a bit so transitions are captured, then de-escalate is
# bounded by WINDOW. We inspect markers after the daemon flushes (on stop),
# but the START marker is written at escalate time and a sync flush happens on
# block rotation; dump_markers reads whatever is on disk. Give it a moment.
sleep "$WINDOW"

# ── 4. bounded window auto-de-escalates ───────────────────────
DEESC=1
for _ in $(seq 1 40); do
    TIER=$(ctl '{"cmd":"status"}' | jget "['tier']" 2>/dev/null)
    if [[ "$TIER" == "sampled" ]]; then DEESC=0; break; fi
    sleep 0.5
done
check $DEESC "bounded anomaly window auto-de-escalated back to sampled (tier=$TIER)"

reap_storm

# ── 5. cooldown blocks an immediate re-fire ───────────────────
echo "  injecting second storm inside cooldown..."
DROP0=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_dropped_cooldown_total']")
FIRES_BEFORE=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_fires_total']")
start_holder 8
sleep 0.5
start_storm 12 6
sleep 4   # well inside the $COOLDOWN window
FIRES_AFTER=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_fires_total']")
DROP1=$(ctl '{"cmd":"metrics"}' | jget "['anomaly_dropped_cooldown_total']")
TIER=$(ctl '{"cmd":"status"}' | jget "['tier']")
echo "  cooldown: fires $FIRES_BEFORE->$FIRES_AFTER drops $DROP0->$DROP1 tier=$TIER"
[[ "$FIRES_AFTER" == "$FIRES_BEFORE" ]]
check $? "no re-fire inside cooldown (fires unchanged at $FIRES_AFTER)"
[[ "$DROP1" -gt "$DROP0" ]]
check $? "cooldown suppressed the second storm (drops $DROP0 -> $DROP1)"
reap_storm

# ── flush + inspect the trace for the ANOMALY marker ──────────
kill -TERM "$TRACER_PID"; wait "$TRACER_PID" 2>/dev/null; TRACER_PID=""

if [[ -x "$DUMP" ]]; then
    MARKERS=$("$DUMP" "$TRACE_DIR" 2>/dev/null)
    echo "$MARKERS" | sed 's/^/    /'
    [[ "$MARKERS" == *"START reason=anomaly"* ]]
    check $? "trace records a START reason=anomaly escalation marker"
    # The manual escalate is NOT used here, so no manual marker should appear.
    if [[ "$MARKERS" == *"reason=manual"* ]]; then
        check 1 "anomaly window distinct from manual (unexpected manual marker)"
    else
        check 0 "anomaly window distinct from manual (no manual markers present)"
    fi
else
    echo "  SKIP: dump_markers not built — cannot decode markers"
fi

echo ""
echo "$passed/$((passed + failed)) tests passed"
[[ $failed -eq 0 ]]
