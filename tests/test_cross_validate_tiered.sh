#!/bin/bash
# test_cross_validate_tiered.sh — A4 cross-validation (sampled vs exact).
#
# THE key A4 test. Runs the daemon in --mode tiered under a pgbench workload,
# escalates to full fidelity for a window, then compares the sampled
# estimators against the exact transition data over the SAME window using the
# cross_validate tool. Used to choose/justify the default --sample-rate.
#
# Sweeps a list of sample rates so the operator can see which rate hits the
# +/-10pp tolerance on the workload. The first rate that PASSES is reported as
# the recommended default.
#
# Requires: root, running PostgreSQL, pgbench, cross_validate built.
# Usage: sudo tests/test_cross_validate_tiered.sh [--pid PID] [--rates "10 50 100"]
#                                                 [--esc-seconds 60] [--clients 8]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
XVAL="$SCRIPT_DIR/cross_validate"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
RATES="10 50 100 200"
ESC_SECONDS=60
CLIENTS=8
TOLERANCE=10

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --rates) RATES="$2"; shift 2 ;;
        --esc-seconds) ESC_SECONDS="$2"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --tolerance) TOLERANCE="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid PID] [--rates \"10 50 100\"] [--esc-seconds N] [--clients N]"; exit 1 ;;
    esac
done

[[ -z "$PM_PID" ]] && PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then echo "ERROR: cannot find postmaster PID"; exit 1; fi
if [[ ! -x "$XVAL" ]]; then echo "ERROR: build tests/cross_validate first (make -C tests cross_validate)"; exit 1; fi

echo "=== test_cross_validate_tiered (PID $PM_PID) ==="
echo "Rates: $RATES   escalation: ${ESC_SECONDS}s   clients: $CLIENTS"

ctl() {
    python3 - "$1" "$2" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf=b""
while b"\n" not in buf:
    c=s.recv(4096)
    if not c: break
    buf+=c
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

# Ensure pgbench schema exists
pgbench -U postgres -d postgres -i -s 10 >/dev/null 2>&1 || true

best_rate=""
declare -A rate_result

for RATE in $RATES; do
    echo ""
    echo "----- sample-rate ${RATE} Hz -----"
    TRACE_DIR=$(mktemp -d /tmp/pgwt_xval_XXXXXX)
    SOCK="$TRACE_DIR/pgwt.sock"
    LOG=$(mktemp /tmp/pgwt_xval_log_XXXXXX)

    "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
        --mode tiered --sample-rate "$RATE" --escalation-budget 600 -q \
        >/dev/null 2>"$LOG" &
    TPID=$!

    for _ in $(seq 1 30); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
    if [[ ! -S "$SOCK" ]]; then echo "  daemon failed to start"; tail "$LOG"; kill "$TPID" 2>/dev/null; rm -rf "$TRACE_DIR" "$LOG"; continue; fi

    # Start workload
    pgbench -U postgres -d postgres -c "$CLIENTS" -j 2 -T $((ESC_SECONDS + 8)) \
        >/dev/null 2>&1 &
    PGB=$!
    sleep 3   # warm up the sampler before escalating

    # Escalate for the window
    RESP=$(ctl "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cross-validate\"}")
    echo "  escalate: $RESP"

    sleep $((ESC_SECONDS + 3))   # let the window run + expire

    # Stop workload + daemon (flushes trace)
    kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
    kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

    # Compare
    OUT=$("$XVAL" "$TRACE_DIR" --tolerance "$TOLERANCE")
    echo "$OUT" | sed 's/^/  /'
    if [[ "$OUT" == *"RESULT: PASS"* ]]; then
        rate_result[$RATE]="PASS"
        [[ -z "$best_rate" ]] && best_rate="$RATE"
    else
        rate_result[$RATE]="FAIL"
    fi

    rm -rf "$TRACE_DIR" "$LOG"
done

echo ""
echo "===== Cross-validation summary ====="
for RATE in $RATES; do
    printf "  %4s Hz : %s\n" "$RATE" "${rate_result[$RATE]:-?}"
done
if [[ -n "$best_rate" ]]; then
    echo "Recommended default --sample-rate: ${best_rate} Hz (first rate within +/-${TOLERANCE}pp)"
    exit 0
else
    echo "No tested rate met +/-${TOLERANCE}pp tolerance — see per-rate tables above."
    exit 1
fi
