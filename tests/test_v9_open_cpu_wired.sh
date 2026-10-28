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
CPU_LOOP_SQL='DO $$ DECLARE s bigint := 0; i bigint; BEGIN FOR i IN 1..40000000 LOOP s := s + i; END LOOP; END $$;'
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
MEAS=$(num "$M1" summary_open_cpu_measured_ns)
REFUSED=$(num "$M1" summary_open_cpu_refused_total)
UNCRED=$(num "$M1" summary_cpu_open_uncredited_ns_total)
IOREF=$(num "$M1" summary_open_io_worker_refused_total)
IOREF_CPU=$(num "$M1" summary_open_cpu_io_worker_refused_total)
UNPLACED=$(num "$M1" summary_open_cpu_unplaced_total)
TBLFULL=$(num "$M1" summary_open_table_full_total)
EXPIRED=$(num "$M1" summary_open_notes_expired_total)
V8CRED=$(num "$M1" summary_open_credited_ns_total)
echo "  summary_open_cpu_credited_ns   = $CRED"
echo "  summary_open_cpu_measured_ns   = ${MEAS:-n/a}"
echo "  summary_open_cpu_refused_total = ${REFUSED:-n/a}"
echo "  summary_cpu_open_uncredited_ns_total = ${UNCRED:-n/a}"
# Every exit between the note and the credit, so a verdict NAMES the one
# that took the observation instead of leaving it to be inferred.
echo "  summary_open_io_worker_refused_total = ${IOREF:-n/a}  (WAIT notes included)"
echo "  summary_open_cpu_io_worker_refused_total = ${IOREF_CPU:-n/a}  (the on-CPU subset)"
echo "  summary_open_cpu_unplaced_total      = ${UNPLACED:-n/a}"
echo "  summary_open_table_full_total        = ${TBLFULL:-n/a}"
echo "  summary_open_notes_expired_total     = ${EXPIRED:-n/a}"
echo "  summary_open_credited_ns_total = ${V8CRED:-n/a}  (WAIT credit, same seal)"

# The two keys that make a zero ATTRIBUTABLE are preconditions, not
# decoration: without them this script is back to inferring which of three
# states a zero is, which is the mistake of 2026-10-28. A daemon that
# predates them cannot be read, and will not be guessed at.
if [[ -z "$IOREF_CPU" ]] || [[ -z "$UNPLACED" ]]; then
    echo "  REFUSE: metrics has no numeric"
    echo "          'summary_open_cpu_io_worker_refused_total' /"
    echo "          'summary_open_cpu_unplaced_total'. Without the first,"
    echo "          summary_open_io_worker_refused_total overlaps"
    echo "          notes_total by an unknown amount (34 notes beside 39"
    echo "          refusals on the 2026-10-28 capture); without the second,"
    echo "          'every gap ended inside its own second' and 'the seal"
    echo "          dropped them' are the same three zeros. This daemon"
    echo "          predates both and cannot name a defect."
    exit 2
fi

# What the CREDIT PATH was actually offered: notes minus the ones the
# io_worker rule declined at the note. Subtraction, not inference.
OFFERED=$(( NOTES - IOREF_CPU ))
echo "  offered to the credit path     = $OFFERED  (notes $NOTES - io_worker $IOREF_CPU)"

if [[ "$CRED" -gt 0 ]]; then
    # ── A NUMBER, not "greater than zero" ───────────────────────────────
    # "credited > 0" is the weakest assertion this capture can carry, and
    # one second of credit out of a ten-second gap satisfies it -- i.e. a
    # credit path that places the FIRST second and then stops passes it.
    # So the verdict is a band, derived from quantities this same run
    # measured rather than from a constant someone chose:
    #
    #   (1) measured > 0            v7's split survived the spread. A
    #                               credit that places wall and no
    #                               measurement destroys the CPU*/Off-CPU*
    #                               split, which is the whole reason v8
    #                               refused to spread on-CPU gaps at all.
    #   (2) measured <= wall        coherence. A second cannot hold more
    #                               measured CPU than its own wall; the
    #                               reverse makes the reader's
    #                               DB - CPU* - Sigma residual negative.
    #   (3) 4 * measured >= DELTA   the BULK of the stimulus, not a token
    #                               second. DELTA is this daemon's own
    #                               schedstat total across the stimulus
    #                               (cpu_ns_total, independent provenance
    #                               from the credit). A gap of L seconds
    #                               can place about L - 2.1 of them (the
    #                               second must complete, plus
    #                               PGWT_SUMMARY_FLUSH_LAG_NS, plus the
    #                               first partial second, which rides the
    #                               closing record instead) -- so at the
    #                               >= 3 s floor enforced above the placed
    #                               fraction is already ~30%, and DELTA
    #                               also carries the aux backends' CPU,
    #                               which only makes the denominator
    #                               bigger. 25% leaves that headroom while
    #                               still rejecting a one-second credit of
    #                               a ten-second gap (10%).
    DELTA=$(( CPU1 - CPU0 ))
    bad=0
    if [[ -z "$MEAS" ]] || [[ "$MEAS" -le 0 ]]; then
        echo "  FAIL: credited $CRED ns of WALL with measured CPU"
        echo "        ${MEAS:-n/a} -- the spread placed time and destroyed"
        echo "        v7's measured split, which is worse than refusing to"
        echo "        spread. src/summary_writer.c credit_open_cpu_measured."
        bad=1
    elif [[ "$MEAS" -gt "$CRED" ]]; then
        echo "  FAIL: measured $MEAS ns > wall $CRED ns. A second cannot"
        echo "        hold more measured CPU than its own wall; this makes"
        echo "        the reader's DB - CPU* - Sigma residual negative."
        bad=1
    elif (( MEAS * 4 < DELTA )); then
        echo "  FAIL: measured $MEAS ns is under a quarter of the"
        echo "        stimulus' own $DELTA ns of schedstat CPU. The credit"
        echo "        path placed a token amount, not the gap -- which is"
        echo "        what 'credited > 0' alone would have approved."
        bad=1
    fi
    if [[ "$bad" -ne 0 ]]; then
        echo "=== test_v9_open_cpu_wired: FAIL ==="
        exit 1
    fi
    echo "  ok: the v9 open-CPU producer IS wired and the credit is coherent"
    echo "      wall $CRED ns, measured $MEAS ns, stimulus CPU $DELTA ns"
    echo "      (measured/stimulus = $(( MEAS * 100 / (DELTA > 0 ? DELTA : 1) ))%, floor 25%)"
    echo "=== test_v9_open_cpu_wired: PASS ==="
    exit 0
fi

# ── credited == 0. Four states, and each names a different file. ─────────
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

if [[ "$OFFERED" -le 0 ]]; then
    echo "  REFUSE: all $NOTES on-CPU observations carried the io_worker"
    echo "          flag ($IOREF_CPU of $NOTES), and io_worker time is"
    echo "          outside the time model by rule (summary v6), so"
    echo "          declining them is CORRECT. The credit path was offered"
    echo "          nothing. On a PG18 io_method=worker cluster the io"
    echo "          workers are the only backends with an on-CPU gap when"
    echo "          the stimulus itself does not hold one -- so this says"
    echo "          the stimulus failed, not the writer."
    exit 2
fi

if [[ "$UNPLACED" -gt 0 ]]; then
    echo "  REFUSE: the credit path was offered $OFFERED observations and"
    echo "          retired $UNPLACED intervals without writing a single"
    echo "          second of any of them -- every on-CPU gap in this"
    echo "          capture ended INSIDE the second it was noted in. v9"
    echo "          spreads an interval across the seconds it occupies, so"
    echo "          there was nothing to spread and a zero is the CORRECT"
    echo "          answer. The STIMULUS is what has to change, not the"
    echo "          writer: a statement that burns CPU is not the same"
    echo "          thing as one that holds an on-CPU interval open across"
    echo "          a tick boundary (a FunctionScan over generate_series"
    echo "          spills a tuplestore and changes wait event thousands of"
    echo "          times a second; a plpgsql loop does not)."
    exit 2
fi

echo "  FAIL: summary_open_cpu_credited_ns is 0 on a capture whose CPU"
echo "        stimulus is proven, with the v9 keys present, with"
echo "        summary_open_credited_ns_total = ${V8CRED:-n/a} proving seconds"
echo "        really were sealed, with $OFFERED observations OFFERED to the"
echo "        credit path after the io_worker rule, and with"
echo "        summary_open_cpu_unplaced_total = 0 -- so not one of them was"
echo "        retired early either. They were accepted and the SEAL placed"
echo "        none of them: src/summary_writer.c credit_open_second /"
echo "        writer_seal_through. refused_total=${REFUSED:-n/a}"
echo "        table_full=${TBLFULL:-n/a} notes_expired=${EXPIRED:-n/a}"
echo "        uncredited_ns=${UNCRED:-n/a} is what it cost."
echo "=== test_v9_open_cpu_wired: FAIL ==="
exit 1
