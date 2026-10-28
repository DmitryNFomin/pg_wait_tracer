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
#   - WATCHPOINT PROBES ARE ATTACHED (exact_probe_attached_mask > 0). Note
#     that this is NOT "tier == exact", which is what this header claimed
#     until 2026-10-28 while the code below checked the mask -- see the
#     comment at the check itself for why the mask is the fact and the tier
#     label is not. The producer lives in pgwt_read_state_map, which only
#     runs when the mode maintains watchpoint intervals; without them a zero
#     is CORRECT, so asserting non-zero there would fail a healthy daemon.
#   - the control socket answers `metrics`.
#   - the seven v9 keys are present.
#   - the stimulus is PROVEN: a CPU-only statement must actually have been
#     on-CPU across at least two ticks, evidenced by cpu_ns_total moving.
#     Without that the assertion would be about the workload, not the wiring.
#   - seconds really were SEALED (summary_open_credited_ns_total > 0), else
#     nothing could have been credited by anyone.
#   - THE WRITER WAS ASKED (summary_open_cpu_notes_total > 0). Added
#     2026-10-28 after this script printed "the producer is not wired" on a
#     capture where that was not established: a zero credited_ns is BOTH
#     "the producer never offered an observation" and "it offered one every
#     tick and the credit path dropped it", and until this counter existed
#     nothing on the wire separated them.
# Only once ALL of those hold does a zero count as a FAILURE (exit 1).
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

echo "=== test_v9_open_cpu_wired (#424, summary v9 producer) ==="

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
    echo "        ${MASK:-absent}) after 20 s. The producer loop is"
    echo "        unreachable here, so a zero would be correct and this"
    echo "        check would be measuring nothing."
    echo "  status: $STATUS"
    exit 2
fi
TIER=$(printf '%s' "$STATUS" | python3 -c \
    "import json,sys;print(json.load(sys.stdin).get('tier',''))" 2>/dev/null)
echo "  ok: watchpoints attached (mask=$MASK, tier=$TIER) -- producer reachable"

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
# EVERY leg is bounded. The first version of this block ran three
# unbounded psql children and then a bare `wait`, and the whole test was
# killed by its harness at 400 s with no verdict -- a HANG, which is the
# one outcome worse than a failure because it reports nothing. A psql that
# blocks on authentication, or a generate_series sized for a faster box,
# produces exactly that. Each child now self-terminates, and the stimulus
# not completing is a REFUSAL, not a silent pass.
STIM_PIDS=""
STIM_T0=$(date +%s)
for _ in 1 2 3; do
    timeout 45 psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM generate_series(1,60000000);" >/dev/null 2>&1 &
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

M1=$(ctl '{"cmd":"metrics"}')
CPU1=$(num "$M1" cpu_ns_total)
if [[ -z "$CPU1" ]] || [[ "$CPU1" -le "$CPU0" ]]; then
    echo "REFUSE: cpu_ns_total did not move ($CPU0 -> ${CPU1:-?})."
    echo "        The CPU statement never registered, so a zero credited_ns"
    echo "        would be a statement about the workload, not the wiring."
    exit 2
fi
echo "  ok: stimulus PROVEN -- cpu_ns_total moved $CPU0 -> $CPU1"

# ── THE FIFTH PRECONDITION: was the writer ASKED? ───────────────────────
# Read from the SAME metrics object as the counter below, because the two
# have to describe one instant. summary_open_cpu_notes_total counts on-CPU
# notes that arrived carrying an observation, before any refusal inside the
# writer -- so:
#
#   key absent  -> this daemon predates the counter. This script cannot tell
#                  "never asked" from "asked and dropped", which is exactly
#                  the verdict it would otherwise print. REFUSE.
#   0           -> the PRODUCER never offered an observation. credited == 0
#                  is then the correct answer and says nothing about the
#                  credit path; the defect is the call site. REFUSE.
#   > 0         -> the writer was asked. Only now does a zero credited_ns
#                  mean the credit path placed nothing. FAIL.
#
# This is why the run of 2026-10-28 (box-check-ubuntu-20261028-121033,
# commit 31a03da6) was misread: all four preconditions above held and were
# printed, yet credited / refused / uncredited were ALL zero, which is both
# states at once. No sleep and no retry is added -- the number either is on
# the wire at the instant the counter is read or it is not.
NOTES=$(num "$M1" summary_open_cpu_notes_total)
if [[ -z "$NOTES" ]]; then
    echo "  REFUSE: metrics has no numeric 'summary_open_cpu_notes_total'."
    echo "          Without it a zero credited_ns is BOTH 'the producer never"
    echo "          offered an observation' and 'it did and nothing was"
    echo "          credited'. This daemon predates the counter, so this"
    echo "          check cannot name a defect and will not guess one."
    exit 2
fi
if [[ "$NOTES" -le 0 ]]; then
    echo "  REFUSE: summary_open_cpu_notes_total = 0. The writer was never"
    echo "          ASKED about an on-CPU gap, so credited_ns == 0 is the"
    echo "          CORRECT answer and the credit path was never exercised."
    echo "          The defect is upstream, at the producer call site in"
    echo "          pgwt_read_state_map (src/map_reader.c) or in what the"
    echo "          state-map scan sees -- not in the writer."
    exit 2
fi
echo "  ok: the writer WAS asked -- summary_open_cpu_notes_total = $NOTES"

CRED=$(num "$M1" summary_open_cpu_credited_ns)
REFUSED=$(num "$M1" summary_open_cpu_refused_total)
UNCRED=$(num "$M1" summary_cpu_open_uncredited_ns_total)
IOREF=$(num "$M1" summary_open_io_worker_refused_total)
TBLFULL=$(num "$M1" summary_open_table_full_total)
EXPIRED=$(num "$M1" summary_open_notes_expired_total)
echo "  summary_open_cpu_credited_ns   = $CRED"
echo "  summary_open_cpu_refused_total = ${REFUSED:-n/a}"
echo "  summary_cpu_open_uncredited_ns_total = ${UNCRED:-n/a}"
# The three silent early exits between the note and the credit, so a FAIL
# names which one ate the observation instead of leaving it to be inferred.
echo "  summary_open_io_worker_refused_total = ${IOREF:-n/a}"
echo "  summary_open_table_full_total        = ${TBLFULL:-n/a}"
echo "  summary_open_notes_expired_total     = ${EXPIRED:-n/a}"

# THREE ZEROS HAVE TWO MEANINGS, and this script originally reported only
# one of them. agent/424-daemon (dc23287) found the fourth outcome by
# reading the writer: crediting happens only in writer_seal_through(),
# whose first statement is `if (!w->fp) return;` and whose only callers are
# pgwt_summary_push_event and pgwt_summary_flush_completed. A capture whose
# in-flight statements are pure CPU emits no wait boundaries, so nothing is
# pushed, nothing seals, and credited/refused/uncredited all stay 0 even
# with a perfectly wired producer. "The producer never ran" is what I read
# three zeros as, and it was not established.
#
# summary_open_credited_ns_total -- the v8 WAIT crediting, which goes
# through the same seal -- is the discriminator, so this script now reports
# it instead of leaving the next reader to make the same inference.
V8CRED=$(num "$M1" summary_open_credited_ns_total)
echo "  summary_open_credited_ns_total = ${V8CRED:-n/a}  (the discriminator)"

if [[ "$CRED" -gt 0 ]]; then
    echo "  ok: the v9 open-CPU producer IS wired (credited $CRED ns)"
    echo "=== test_v9_open_cpu_wired: PASS ==="
    exit 0
fi

if [[ -n "$V8CRED" ]] && [[ "$V8CRED" -eq 0 ]]; then
    echo "  REFUSE: summary_open_cpu_credited_ns is 0, but so is"
    echo "          summary_open_credited_ns_total -- NOTHING sealed in this"
    echo "          capture, so the open-CPU path was never reached and this"
    echo "          says nothing about whether the producer is wired. Fix the"
    echo "          seal trigger (writer_seal_through is only called from"
    echo "          push_event and flush_completed, and returns immediately"
    echo "          while w->fp is NULL) before reading this check again."
    exit 2
fi

echo "  FAIL: summary_open_cpu_credited_ns is 0 on a capture whose CPU"
echo "        statement is proven, with the v9 keys present, with"
echo "        summary_open_credited_ns_total = ${V8CRED:-n/a} proving seconds"
echo "        really were sealed, AND with the writer ASKED $NOTES times"
echo "        (summary_open_cpu_notes_total). So the observation reached the"
echo "        writer and the CREDIT PATH placed none of it -- which is a"
echo "        different defect from an unwired producer, and the counter"
echo "        above is what distinguishes them. Which early exit took it:"
echo "        io_worker=${IOREF:-n/a} table_full=${TBLFULL:-n/a}"
echo "        notes_expired=${EXPIRED:-n/a} eff_rejected/overcredit above."
echo "        All of those zero narrows it to the seal placing no second."
echo "        refused_total=${REFUSED:-n/a}"
echo "        uncredited_ns=${UNCRED:-n/a} is what it cost."
echo "=== test_v9_open_cpu_wired: FAIL ==="
exit 1
