#!/bin/bash
# test_v9_open_cpu_wired.sh — summaries must stay off on a real capture.
#
# The producer-wiring check comes back when summaries are rebuilt from the
# core: docs/REWORK_PLAN_2026-11-11.md section 5, "Rollups (summaries)".
# Until then, a live daemon must leave summary_open_cpu_notes_total and
# summary_open_credited_ns_total at 0 before and after a proven CPU stimulus,
# and must not create .summary or .summary.meta files in the capture directory.
#
# PRECONDITIONS, each checked and each a REFUSAL (exit 2) rather than a pass:
#   - watchpoint probes are attached (exact_probe_attached_mask > 0).
#   - the control socket answers `metrics` successfully.
#   - the v9 keys and writer-activity counters are present and numeric.
#   - CPU statements complete across ticks and cpu_ns_total moves.
# Only after those hold can writer activity be a FAILURE (exit 1).
#
# NOTE ON EXIT 2, because the distinction is only in this script's output:
# tests/run_all.sh counts any non-zero exit as a failure, so a REFUSE turns
# the gate red exactly like a FAIL. The banner line (REFUSE vs FAIL) is the
# only place the two are told apart; read it, do not read the pass counts.
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
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"
           echo "=== test_v9_open_cpu_wired: REFUSE ==="; exit 2 ;;
    esac
done
[[ -n "$PM_PID" ]] || PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then
    echo "REFUSE: cannot find postmaster PID"
    # Explicit, not via the trap below: this exit happens before the trap is
    # installed, because cleanup() needs TRACE_DIR to exist.
    echo "=== test_v9_open_cpu_wired: REFUSE ==="
    exit 2
fi

echo "=== test_v9_open_cpu_wired (summaries off until core rebuild) ==="

TRACE_DIR=$(mktemp -d /tmp/pgwt_v9wired_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_v9wired_daemon_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""

cleanup() {
    local rc=$?
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null
        wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$TRACE_DIR" "$DAEMON_LOG"
    # EVERY refusal gets the banner, including the ones that exit before
    # reaching a verdict (no daemon, no socket, no probes, no stimulus).
    # tests/run_all.sh counts exit 2 as a failure exactly like exit 1, so
    # this banner is the ONLY thing in the gate log that says "this was a
    # refusal, not a product defect" -- and six of the refusal sites below
    # printed their reason and then exited silently, which is how a red
    # refusal gets read as a red product. Emitted from the trap rather than
    # repeated at each site so a refusal added later cannot forget it.
    if [[ "$rc" -eq 2 ]]; then
        echo "=== test_v9_open_cpu_wired: REFUSE ==="
    fi
    return "$rc"
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

MASK_PY="
import json, sys
try:
    r = json.load(sys.stdin)
except Exception:
    print(0); raise SystemExit
v = r.get('exact_probe_attached_mask')
print(v if isinstance(v, int) else 0)
"

num() {  # num <json> <key>  -> the numeric value, or the empty string
    printf '%s' "$1" | python3 -c "
import json, sys
try:
    r = json.load(sys.stdin)
except Exception:
    sys.exit(0)
v = r.get('$2')
print(v if type(v) is int and v >= 0 else '')
"
}

# ── full mode, so watchpoint intervals are exercised ───────────────────
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

# The precondition is "watchpoint intervals exist", because that is what
# gates pgwt_read_state_map and therefore the producer. It is NOT
# tier == "exact": a `--mode full` daemon on the gate box reports
# tier="escalated" with exact_probe_attached_mask=3, and the first version
# of this check refused on exactly that -- loudly, which is the only reason
# the mistake was visible instead of becoming a permanent silent skip. The
# mask is the fact; the tier label describes how the daemon got there.
STATUS=""
MASK=0
for _ in $(seq 1 20); do
    STATUS=$(ctl '{"cmd":"status"}')
    MASK=$(printf '%s' "$STATUS" | python3 -c "$MASK_PY" 2>/dev/null)
    [[ -n "$MASK" ]] && [[ "$MASK" -gt 0 ]] && break
    sleep 1
done
if [[ -z "$MASK" ]] || [[ "$MASK" -le 0 ]]; then
    echo "REFUSE: no watchpoint probes attached (exact_probe_attached_mask="
    echo "        ${MASK:-absent}) after 20 s. The exact interval loop is"
    echo "        unreachable here, so a zero would be correct and this"
    echo "        check would be measuring nothing."
    echo "  status: $STATUS"
    exit 2
fi
TIER=$(printf '%s' "$STATUS" | python3 -c \
    "import json,sys;print(json.load(sys.stdin).get('tier',''))" 2>/dev/null)
echo "  ok: watchpoints attached (mask=$MASK, tier=$TIER) -- exact interval loop reachable"

# Check the same keys at both ends: a missing counter must not look like 0.
SUMMARY_KEYS=(
    summary_open_cpu_credited_ns summary_open_cpu_measured_ns
    summary_open_cpu_unknown_ns summary_open_cpu_eff_rejected_total
    summary_open_cpu_class_divergent_ns summary_open_cpu_tier_mismatch_total
    summary_open_cpu_overcredit_ns summary_open_cpu_notes_total
    summary_open_credited_ns_total summary_open_cpu_io_worker_refused_total
    summary_open_cpu_unplaced_total
)
check_metrics() {
    if ! printf '%s' "$1" | python3 -c '
import json, sys
try:
    r = json.load(sys.stdin)
    sys.exit(0 if isinstance(r, dict) and r.get("ok") is True else 1)
except (ValueError, TypeError):
    sys.exit(1)
'; then
        echo "REFUSE: metrics did not return a successful JSON response"
        exit 2
    fi
    local k
    for k in cpu_ns_total "${SUMMARY_KEYS[@]}"; do
        if [[ -z "$(num "$1" "$k")" ]]; then
            echo "REFUSE: metrics has no non-negative integer '$k'"
            exit 2
        fi
    done
}

M0=$(ctl '{"cmd":"metrics"}') || {
    echo "REFUSE: metrics request failed before the stimulus"
    exit 2
}
check_metrics "$M0"
CPU0=$(num "$M0" cpu_ns_total)
echo "  ok: metrics answers and all required counters are present and numeric"

# ── the stimulus: ONE on-CPU interval per backend, held open across
#    several 1 s ticks, because that is the only thing v9 can spread. ─────
#
# NOT `SELECT count(*) FROM generate_series(1,60000000)`, which this script
# used until 2026-10-28 and which does not produce that interval at all. A
# set-returning function in FROM is a FunctionScan, and FunctionScan
# MATERIALISES its result into a tuplestore: 60 M rows is ~1.5 GB against a
# 4 MB work_mem, so the backend spends the statement writing temp files and
# changes wait event thousands of times a second. Every on-CPU stretch is
# tens of microseconds long, there is no interval open across a second
# boundary, and a zero open-CPU credit is then the CORRECT answer.
#
# The gate capture of 2026-10-28 (box-check-ubuntu-20261028-154345.log) says
# so in its own numbers: 34 on-CPU notes, 0 credited, every refusal counter
# 0, and -- the one that settles it -- summary_cpu_open_uncredited_ns_total
# also 0. That counter adds the WHOLE duration of every closed record whose
# class is CPU and whose duration exceeds one second that no second had
# credited. Zero, on a capture that burned 34 s of measured CPU, means not
# one on-CPU interval longer than a second CLOSED in it. The statement was
# proven to burn CPU; it was never proven to hold a gap, and those are
# different facts.
#
# A plpgsql FOR loop over a scalar assignment has no rows, no tuplestore, no
# temp file and no wait event: one interval, open from the first iteration to
# the last. Sized so it exceeds the >= 3 s check below on the gate box while
# staying well inside its own timeout; the dollar quoting is single-quoted so
# bash cannot expand $$ into its PID.
# SIZED BY THE CLOCK, not by an iteration count. The first version of this
# loop ran `FOR i IN 1..40000000`, which the gate box finished in 2 s --
# under the >= 3 s floor below, so the fixture REFUSED (correctly: a stimulus
# that cannot span a tick boundary measures nothing, and plpgsql turns out to
# do ~20 M scalar assignments a second on that machine). A constant tuned to
# one box is a stimulus that silently stops spanning ticks the next time the
# hardware changes, so the loop runs until a DEADLINE instead and is right on
# any box. The clock is read once per 200 k iterations (tens of ms) so the
# read itself is not the workload; clock_timestamp() is a plain gettimeofday
# and sets no wait event.
CPU_LOOP_SECONDS=9
CPU_LOOP_SQL="DO \$\$
DECLARE
    s bigint := 0;
    i bigint := 0;
    deadline timestamptz := clock_timestamp()
                            + interval '$CPU_LOOP_SECONDS seconds';
BEGIN
    LOOP
        i := i + 1;
        s := s + i;
        IF i % 200000 = 0 AND clock_timestamp() > deadline THEN
            EXIT;
        END IF;
    END LOOP;
END \$\$;"
# EVERY leg is bounded. The first version of this block ran three
# unbounded psql children and then a bare `wait`, and the whole test was
# killed by its harness at 400 s with no verdict -- a HANG, which is the
# one outcome worse than a failure because it reports nothing. A psql that
# blocks on authentication, or a loop sized for a faster box, produces
# exactly that. Each child now self-terminates, and the stimulus not
# completing is a REFUSAL, not a silent pass.
STIM_PIDS=""
STIM_T0=$(date +%s)
for _ in 1 2 3; do
    timeout 120 psql -U postgres -d postgres -tAc "$CPU_LOOP_SQL" \
        >/dev/null 2>&1 &
    STIM_PIDS="$STIM_PIDS $!"
done
stim_ok=0
for pid in $STIM_PIDS; do
    if wait "$pid"; then stim_ok=$((stim_ok + 1)); fi
done
if [[ "$stim_ok" -eq 0 ]]; then
    echo "REFUSE: no CPU statement completed (0 of 3; psql missing, refused"
    echo "        a connection, or timed out). Nothing was measured."
    exit 2
fi
STIM_S=$(( $(date +%s) - STIM_T0 ))
# v9 credits an OPEN interval PER SECOND, so a statement that begins and
# ends inside one second has nothing to spread and a zero would be correct.
# The stimulus must therefore be on-CPU ACROSS tick boundaries, and that --
# not merely "cpu_ns_total moved" -- is what has to be proven. 5 M rows ran
# in ~0.75 s and proved nothing; 60 M spans several seconds.
if [[ "$STIM_S" -lt 3 ]]; then
    echo "REFUSE: the CPU statements finished in ${STIM_S}s, too fast to"
    echo "        span the tick boundaries v9 credits across. A zero would"
    echo "        be correct and this check would be measuring nothing."
    exit 2
fi
echo "  ok: $stim_ok of 3 CPU statements completed, spanning ${STIM_S}s of ticks"
sleep 3   # let the ticks after the statements land

M1=$(ctl '{"cmd":"metrics"}') || {
    echo "REFUSE: metrics request failed after the stimulus"
    exit 2
}
check_metrics "$M1"
CPU1=$(num "$M1" cpu_ns_total)
if [[ "$CPU1" -le "$CPU0" ]]; then
    echo "REFUSE: cpu_ns_total did not move ($CPU0 -> $CPU1)."
    echo "        The CPU statement never registered, so this check cannot"
    echo "        establish that summaries stayed off under CPU load."
    exit 2
fi
echo "  ok: stimulus PROVEN -- cpu_ns_total moved $CPU0 -> $CPU1"

bad=0
for k in summary_open_cpu_notes_total summary_open_credited_ns_total; do
    before=$(num "$M0" "$k")
    after=$(num "$M1" "$k")
    if [[ "$before" -ne 0 ]] || [[ "$after" -ne 0 ]]; then
        echo "  FAIL: $k is $before -> $after; summaries must stay off"
        bad=1
    else
        echo "  ok: $k stayed 0 -> 0 across the stimulus"
    fi
done

# Include rotated/compressed summaries as well as current.summary and its
# metadata. A failed directory scan is a refusal, not evidence of absence.
SUMMARY_FILES=$(find "$TRACE_DIR" -type f \( -name '*.summary' -o \
    -name '*.summary.meta' -o -name '*.summary.lz4' \)) || {
    echo "REFUSE: cannot inspect the capture directory for summary files"
    exit 2
}
if [[ -n "$SUMMARY_FILES" ]]; then
    echo "  FAIL: summaries must stay off, but capture contains summary files:"
    printf '%s\n' "$SUMMARY_FILES"
    bad=1
else
    echo "  ok: capture directory has no summary files or summary metadata"
fi

if [[ "$bad" -ne 0 ]]; then
    echo "=== test_v9_open_cpu_wired: FAIL ==="
    exit 1
fi
echo "=== test_v9_open_cpu_wired: PASS ==="
exit 0
