#!/bin/bash
# test_v9_open_cpu_wired.sh — is the summary v9 open-CPU PRODUCER actually
# wired, on a real capture?
#
# This is the one #424 failure that still looks green. The v9 writer can be
# in the build, every unit test can pass, every control key can be present
# and numeric, and the live loop can still never hand the writer an
# observation — in which case `summary_open_cpu_credited_ns` stays 0 and
# nothing says so. The counters that make it visible
# (open_cpu_refused_total, cpu_open_uncredited_ns_total) were published
# nowhere before agent B put them on the control socket; before that an
# unwired producer was unobservable, not merely unobserved.
#
# PRECONDITIONS, each checked and each a REFUSAL (exit 2) rather than a pass:
#   - watchpoints available and the daemon reaches tier=exact. The producer
#     lives in pgwt_read_state_map, which only runs when the mode maintains
#     watchpoint intervals; in sampled/lightweight a zero is CORRECT, so
#     asserting non-zero there would fail a healthy daemon.
#   - the control socket answers `metrics`.
#   - the seven v9 keys are present.
#   - the stimulus is PROVEN: a CPU-only statement must actually have been
#     on-CPU across at least two ticks, evidenced by cpu_ns_total moving.
#     Without that the assertion would be about the workload, not the wiring.
# Only once all four hold does a zero count as a FAILURE (exit 1).
#
# Usage: sudo tests/test_v9_open_cpu_wired.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 2 ;;
    esac
done
[[ -n "$PM_PID" ]] || PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then
    echo "REFUSE: cannot find postmaster PID"
    exit 2
fi

echo "=== test_v9_open_cpu_wired (#424, summary v9 producer) ==="

TRACE_DIR=$(mktemp -d /tmp/pgwt_v9wired_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_v9wired_daemon_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""

cleanup() {
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null
        wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$TRACE_DIR" "$DAEMON_LOG"
}
trap cleanup EXIT

ctl() {
    python3 - "$SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
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

num() {  # num <json> <key>  -> the numeric value, or the empty string
    printf '%s' "$1" | python3 -c "
import json, sys
try:
    r = json.load(sys.stdin)
except Exception:
    sys.exit(0)
v = r.get('$2')
print(v if isinstance(v, (int, float)) else '')
"
}

# ── full mode, so the watchpoint interval loop (the producer) runs ───────
"$TRACER" --daemon --mode full --pid "$PM_PID" -i 1 -T "$TRACE_DIR" -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 40); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "REFUSE: daemon exited during startup:"
        tail -20 "$DAEMON_LOG"
        exit 2
    fi
    sleep 0.5
done
if [[ ! -S "$SOCK" ]]; then
    echo "REFUSE: no control socket after 20 s"
    tail -20 "$DAEMON_LOG"
    exit 2
fi

STATUS=$(ctl '{"cmd":"status"}')
TIER=$(printf '%s' "$STATUS" | python3 -c \
    "import json,sys;print(json.load(sys.stdin).get('tier',''))" 2>/dev/null)
if [[ "$TIER" != "exact" ]]; then
    echo "REFUSE: tier is '$TIER', not 'exact' -- the v9 producer only runs"
    echo "        where watchpoint intervals exist, so a zero here would be"
    echo "        correct and this check would be measuring nothing."
    echo "  status: $STATUS"
    exit 2
fi
echo "  ok: daemon is in tier=exact, so the producer loop is reachable"

M0=$(ctl '{"cmd":"metrics"}')
CPU0=$(num "$M0" cpu_ns_total)
if [[ -z "$CPU0" ]]; then
    echo "REFUSE: metrics has no numeric cpu_ns_total -- cannot prove a stimulus"
    exit 2
fi

for k in summary_open_cpu_credited_ns summary_open_cpu_measured_ns \
         summary_open_cpu_unknown_ns summary_open_cpu_eff_rejected_total \
         summary_open_cpu_class_divergent_ns \
         summary_open_cpu_tier_mismatch_total summary_open_cpu_overcredit_ns; do
    if [[ -z "$(num "$M0" "$k")" ]]; then
        echo "REFUSE: control socket has no numeric '$k'."
        echo "        The v9 keys are missing, so this test cannot tell a"
        echo "        wired producer from an unwired one."
        exit 2
    fi
done
echo "  ok: all seven v9 control keys present and numeric"

# ── the stimulus: a statement that is on-CPU, with NO wait event, across
#    several 1 s ticks. generate_series is pure CPU in the backend. ───────
for _ in 1 2 3; do
    psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM generate_series(1,20000000);" >/dev/null 2>&1 &
done
wait
sleep 3   # let the ticks after the statements land

M1=$(ctl '{"cmd":"metrics"}')
CPU1=$(num "$M1" cpu_ns_total)
if [[ -z "$CPU1" ]] || [[ "$CPU1" -le "$CPU0" ]]; then
    echo "REFUSE: cpu_ns_total did not move ($CPU0 -> ${CPU1:-?})."
    echo "        The CPU statement never registered, so a zero credited_ns"
    echo "        would be a statement about the workload, not the wiring."
    exit 2
fi
echo "  ok: stimulus PROVEN -- cpu_ns_total moved $CPU0 -> $CPU1"

CRED=$(num "$M1" summary_open_cpu_credited_ns)
REFUSED=$(num "$M1" summary_open_cpu_refused_total)
UNCRED=$(num "$M1" summary_cpu_open_uncredited_ns_total)
echo "  summary_open_cpu_credited_ns   = $CRED"
echo "  summary_open_cpu_refused_total = ${REFUSED:-n/a}"
echo "  summary_cpu_open_uncredited_ns_total = ${UNCRED:-n/a}"

if [[ "$CRED" -gt 0 ]]; then
    echo "  ok: the v9 open-CPU producer IS wired (credited $CRED ns)"
    echo "=== test_v9_open_cpu_wired: PASS ==="
    exit 0
fi

echo "  FAIL: summary_open_cpu_credited_ns is 0 on a capture whose CPU"
echo "        statement is proven, in tier=exact, with the v9 keys present."
echo "        That is the signature of a v9 writer nobody feeds: the"
echo "        accounting silently falls back to v8 while every other test"
echo "        passes. refused_total=${REFUSED:-n/a}"
echo "        uncredited_ns=${UNCRED:-n/a} is what it cost."
echo "=== test_v9_open_cpu_wired: FAIL ==="
exit 1
