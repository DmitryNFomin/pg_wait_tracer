#!/bin/bash
# test_cross_validate_tiered.sh — A4 cross-validation (sampled vs exact).
#
# THE key A4 test. Runs the daemon in --mode tiered under a pgbench workload,
# escalates to full fidelity for a window, then compares the sampled
# estimators against the exact transition data over the SAME window using the
# cross_validate tool. Used to choose/justify the default --sample-rate.
#
# Sweeps a list of sample rates. Issue #309: this used to exit 0 as soon as
# ANY ONE rate passed, so a run where 3 of 4 rates were outside tolerance
# still reported overall success -- "24/24 green" across several boxes was
# not evidence any given rate, including the shipped default, actually held
# up. Every rate in RATES must now be within tolerance for exit 0; one
# out-of-tolerance rate fails the run (79 only for the bounded #675 signature;
# otherwise 1, including a rate that could not be measured). The first rate
# that PASSES is still printed as the recommended default -- that framing is
# unchanged -- but it
# no longer decides the exit code.
#
# Requires: root, running PostgreSQL, pgbench, cross_validate built.
# Usage: sudo tests/test_cross_validate_tiered.sh [--pid PID] [--rates "10 50 100"]
#                                                 [--esc-seconds 60] [--clients 8]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
XVAL="$SCRIPT_DIR/cross_validate"
source "$SCRIPT_DIR/testutil.sh"
source "$SCRIPT_DIR/cross_validate_rate_output.sh"
source "$SCRIPT_DIR/xval_tolerance.sh"

PM_PID=""
RATES="10 50 100 200"
ESC_SECONDS=60
CLIENTS=8
# +/-8.8 percentage points, DERIVED. Was 10 pp from the A4 tool's original
# commit (eaf31b0) with no derivation anywhere in the repo, and this comment
# used to say so -- "treat 10pp as a value someone should revisit, not a
# measured bound". Issue #309 asked for a derivation; this is it.
#
#   bound = BIAS + z(K) * 100*sqrt(0.25/N)
#         = 6.52 + 2.5688 * 0.8937  =  8.8 pp
#
#   BIAS = 6.52 pp   systematic CPU under-read at the SHIPPED operating point
#                    (#306 arm A median, SCHED_OTHER, 10 Hz, 8 clients). Not
#                    3.9 pp -- that is arm B under SCHED_FIFO, which the
#                    product does not ship.
#   N    = 3130      smallest observed "Samples in window" at that operating
#                    point; samples are (tick, backend) pairs, so a 60 s
#                    10 Hz run yields thousands, not 600.
#   z(5) = 2.5688    95% FAMILY-WISE quantile over the K=5 events that clear
#                    cross_validate's 2% eligibility floor. The gated metric
#                    is a MAXIMUM over those events, so a per-event 2-sigma
#                    figure would cover ~23% of runs, not 95%. #321 was
#                    opened as a "stable 6 pp regression" and withdrawn by
#                    its own author for exactly this reason.
#
# The full derivation, every input's provenance, what the bound does NOT
# cover, and the guard that keeps this constant equal to its derivation live
# in tests/xval_tolerance.sh -- which `make check` runs on every push via
# tests/test_xval_tolerance_derivation.sh. Editing the number here without
# editing the derivation there (or vice versa) turns `make check` red; that
# is deliberate, and is the only reason this comment cannot rot.
#
# Derived for 10 Hz and applied to every rate in the sweep: the published
# post-#310 gate-1 medians fall with rate (6.0 / 6.4 / 5.5 / 2.5 pp at
# 10 / 50 / 100 / 200 Hz), so the 10 Hz bound is the loosest of the four. A
# 200 Hz pass is therefore NOT evidence that rate is inside a bound derived
# for it; that needs its own n>=5.
#
# NOT a certificate of accuracy: 6.52 of the 8.8 pp is #306's OPEN residual
# under-read, which this bound discloses rather than explains.
TOLERANCE=8.8
TOLERANCE_OVERRIDDEN=0

# Pure decision used below and by the offline runner self-test. Each arm is a
# comma-separated list of signed (exact - sampled) eligible-event deviations.
# Exit 79 is reserved here for #675's bounded tolerance-only signature; it is
# distinct from run_all's 78 phase refusal and shell 126/127 and 128+ signals.
# #675 / #760 record 9.8-12.4 pp on EL8 4.18; the two #760 review runs measured
# 9.8-12.4 and 10.1-11.1 pp. 15.0 pp is a regression tripwire for an OPEN
# defect, NOT an accuracy claim or a wider tolerance. Delete it and exit 79
# with the KNOWN_FAILING entry when #675 is fixed.
tiered_classify() {
    python3 - "$@" <<'PYEOF'
import math
import sys

try:
    other_failure, tolerance, *arms = sys.argv[1:]
    tolerance = float(tolerance)
    if other_failure != "0" or not arms or not math.isfinite(tolerance) or tolerance <= 0:
        sys.exit(1)
    outside = False
    peak = 0.0
    for arm in arms:
        for token in arm.split(","):
            delta = float(token)
            if not math.isfinite(delta):
                sys.exit(1)
            peak = max(peak, abs(delta))
            if abs(delta) > tolerance:
                if delta < 0:  # sampled over-reading exact is a different defect
                    sys.exit(1)
                outside = True
    if outside and peak > 15.0:
        sys.exit(1)
    sys.exit(79 if outside else 0)
except ValueError:
    sys.exit(1)
PYEOF
}

# Read ALL eligible event deltas, not just the maximum's unsigned headline.
# cross_validate also gates top-N overlap: a failure there must never be #675.
# Missing/malformed output, a tool error, or inconsistent verdict is INVALID.
tiered_arm_assertions() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import re
import sys

def require(condition):
    if not condition:
        raise ValueError("invalid comparator result")

output, rc, tolerance = sys.argv[1:]
try:
    table = re.search(r"^wait_event\s+exact_share%\s+sampled%\s+delta_pp\n-+\n(.*?)\n\n",
                      output, re.M | re.S)
    require(table)
    deltas = []
    for line in table[1].splitlines():
        row = re.fullmatch(r"\S+\s+([0-9]+\.[0-9]+)%\s+([0-9]+\.[0-9]+)%\s+([0-9]+\.[0-9]+)", line)
        require(row)
        exact, sampled, delta = map(float, row.groups())
        require(0 <= exact <= 100 and 0 <= sampled <= 100)
        require(abs(abs(exact - sampled) - delta) <= 0.11)  # printed to 0.1 pp
        if exact >= 2.0:  # cross_validate's default --min-share
            deltas.append(delta if exact >= sampled else -delta)
    require(deltas)
    overlaps = re.findall(r"^Top-([0-9]+) event overlap: ([0-9]+)/([0-9]+)$", output, re.M)
    require(len(overlaps) == 1)
    top, overlap, denominator = map(int, overlaps[0])
    require(1 <= top <= 5 and denominator == top and min(top, 4) <= overlap <= top)
    maxima = re.findall(r"^Max share disagreement \(events >= 2% exact share\): ([0-9]+\.[0-9]+) pp \(.+\)$", output, re.M)
    require(len(maxima) == 1 and abs(float(maxima[0]) - max(map(abs, deltas))) <= 0.11)
    verdicts = re.findall(r"^RESULT: (PASS|FAIL)$", output, re.M)
    require(len(verdicts) == 1)
    # Around the rounded tolerance boundary, refuse rather than guess why the
    # comparator failed. Its exit code must agree with its printed verdict too.
    passed = max(map(abs, deltas)) <= float(tolerance)
    require(verdicts[0] == ("PASS" if passed else "FAIL"))
    require(rc == ("0" if passed else "1"))
    print(",".join(str(delta) for delta in deltas))
except ValueError:
    print("INVALID")
PYEOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --rates) RATES="$2"; shift 2 ;;
        --esc-seconds) ESC_SECONDS="$2"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --tolerance) TOLERANCE="$2"; TOLERANCE_OVERRIDDEN=1; shift 2 ;;
        *) echo "Usage: $0 [--pid PID] [--rates \"10 50 100\"] [--esc-seconds N] [--clients N]"; exit 1 ;;
    esac
done

[[ -z "$PM_PID" ]] && PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then echo "ERROR: cannot find postmaster PID"; exit 1; fi
if [[ ! -x "$XVAL" ]]; then echo "ERROR: build tests/cross_validate first (make -C tests cross_validate)"; exit 1; fi

echo "=== test_cross_validate_tiered (PID $PM_PID) ==="
echo "Rates: $RATES   escalation: ${ESC_SECONDS}s   clients: $CLIENTS"

# The tolerance this run gates on must be the DERIVED one, not whatever the
# constant above happens to say. `make check` already enforces that on the
# Mac; this re-checks it here because the gate box builds from an rsync'd
# tree and the whole point of the derivation is that the number cannot drift
# away from it unnoticed. An explicit --tolerance is a human override and is
# allowed, but it is announced, so a log can never be read as evidence about
# the derived bound when it was not run against it.
if [[ "$TOLERANCE_OVERRIDDEN" -eq 1 ]]; then
    echo "NOTE: tolerance OVERRIDDEN on the command line to +/-${TOLERANCE}pp;" \
         "this run is NOT evidence about the derived bound" \
         "($(derive_xval_tolerance "$XVAL_PREREG_BIAS_PP" "$XVAL_PREREG_SAMPLES" "$XVAL_PREREG_K" 2>/dev/null || echo '?') pp)."
# $SCRIPT_DIR + basename, not a bare ${BASH_SOURCE[0]}: run_all.sh happens to
# invoke this with an absolute path, so BASH_SOURCE works there, but a caller
# that passed a relative path AND changed directory would make the guard
# unable to read its own file -- and an unreadable file is a REFUSAL, i.e. a
# spurious gate failure for an invocation style that is otherwise fine.
# SCRIPT_DIR is already absolutised at the top of this file.
elif ! DERIVED_TOL=$(xval_tolerance_matches_script "$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"); then
    echo "ERROR: this script's TOLERANCE does not match the pre-registered"
    echo "       derivation in tests/xval_tolerance.sh (see the message above)."
    echo "       Refusing to run: a gate whose threshold it cannot justify"
    echo "       would certify whatever it happens to measure."
    exit 1
else
    echo "Tolerance: +/-${TOLERANCE}pp, derived (BIAS ${XVAL_PREREG_BIAS_PP} pp + z(${XVAL_PREREG_K})*SE at N=${XVAL_PREREG_SAMPLES}) — see tests/xval_tolerance.sh"
fi

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

# Issue #133: this used to run `pgbench -i -s 10` unconditionally on every
# invocation to "ensure the schema exists" -- harmless only because 10
# happened to equal tests/provision-runner.sh's provisioned scale; the
# moment that scale grows, this would silently shrink the shared dataset on
# every run_all, and even at a matching scale `-i` unconditionally drops and
# recreates every pgbench table (stats, bloat, cache state) out from under
# every other test. This test only needs pgbench's normal read/write
# workload against SOME populated pgbench schema -- the row count doesn't
# matter to sampled-vs-exact cross-validation -- so it reads the provisioned
# dataset read-only instead of ever re-initializing it.
ROWS=$(psql -U postgres -d postgres -tAc \
    "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
if [[ "${ROWS:-0}" -lt 1 ]]; then
    echo "ERROR: pgbench_accounts is missing/empty in the shared 'postgres' database."
    echo "       Issue #133: this test reads the provisioned dataset read-only and"
    echo "       never re-initializes it. Run tests/provision-runner.sh (or"
    echo "       'pgbench -i -s 10 -d postgres') to seed it first."
    exit 1
fi

best_rate=""
declare -A rate_result
declare -A rate_delta
declare -A rate_event
declare -A rate_assertions
non_tolerance_failure=0

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
    if [[ ! -S "$SOCK" ]]; then echo "  daemon failed to start"; non_tolerance_failure=1; tail "$LOG"; kill "$TPID" 2>/dev/null; rm -rf "$TRACE_DIR" "$LOG"; continue; fi

    # Start workload
    pgbench -U postgres -d postgres -c "$CLIENTS" -j 2 -T $((ESC_SECONDS + 8)) \
        >/dev/null 2>&1 &
    PGB=$!
    sleep 3   # warm up the sampler before escalating

    # Escalate for the window
    RESP=$(ctl "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cross-validate\"}") || non_tolerance_failure=1
    echo "  escalate: $RESP"

    sleep $((ESC_SECONDS + 3))   # let the window run + expire

    # Stop workload + daemon (flushes trace)
    kill "$PGB" 2>/dev/null
    PGB_RC=0; wait "$PGB" 2>/dev/null || PGB_RC=$?
    [[ "$PGB_RC" -eq 0 || "$PGB_RC" -eq 143 ]] || non_tolerance_failure=1
    kill -TERM "$TPID" 2>/dev/null
    TRACER_RC=0; wait "$TPID" 2>/dev/null || TRACER_RC=$?
    [[ "$TRACER_RC" -eq 0 || "$TRACER_RC" -eq 143 ]] || non_tolerance_failure=1

    # Compare
    XVAL_RC=0
    OUT=$("$XVAL" "$TRACE_DIR" --tolerance "$TOLERANCE") || XVAL_RC=$?
    print_rate_result "$RATE" "$OUT"
    DELTA_INFO=$(extract_max_delta "$OUT")
    rate_delta[$RATE]="${DELTA_INFO%%|*}"
    rate_event[$RATE]="${DELTA_INFO#*|}"

    # Did the comparator see enough of a capture for the derived bound to
    # mean anything? Two distinct refusals, both leaving rate_result UNSET
    # so the summary prints NO RESULT and tiered_classify sees an INVALID arm
    # (the #309 pass-by-omission rule):
    #
    #  1. N absent or unparseable -- the gate is blind. A blind gate must
    #     refuse, never approve; this is the same disposition
    #     extract_max_delta already takes for a missing delta line.
    #  2. N below the DERIVED floor (derive_xval_sample_floor, 2587), which
    #     marks a structurally broken capture -- stalled daemon, mis-aligned
    #     window, dead workload -- rather than ordinary variation. It sits
    #     4.4 sd below the mean of this harness's own capture-thickness
    #     distribution (mean 3102, sd 117, n=11), so ordinary variation
    #     cannot trip it; see the derivation in tests/xval_tolerance.sh.
    #
    #     ROUND 1 GOT THIS WRONG and the correction is the point: the floor
    #     was XVAL_PREREG_SAMPLES (3130), the minimum of a prior n=5 taken
    #     on a DIFFERENT harness, and it fired on 5 of 10 ordinary captures
    #     -- a false failure, not a detection. The tolerance's N and the
    #     validity floor are two different quantities and are now two
    #     different numbers.
    #
    #     Applied at EVERY rate: at 50/100/200 Hz the observed N is
    #     15,718 / 30,527 / 62,864, so the floor cannot cause a false
    #     failure there, but it still catches a broken capture -- which the
    #     previous "any N >= 1" rule at those rates did not.
    SAMP=$(extract_samples_in_window "$OUT")
    if ! SAMP_MIN=$(derive_xval_sample_floor); then
        non_tolerance_failure=1
        echo "  RATE ${RATE}Hz: REFUSED — the capture-thickness floor could not be derived;" \
             "thickness cannot be judged, so this counts as a failure, not a skip"
        rm -rf "$TRACE_DIR" "$LOG"
        continue
    fi
    if ! refuse_unless_samples_sufficient "$SAMP" "$SAMP_MIN" >/dev/null; then
        non_tolerance_failure=1
        echo "  RATE ${RATE}Hz: REFUSED — capture too thin / sample count unreadable" \
             "(N=${SAMP}, need >=${SAMP_MIN}); counts as a failure, not a skip"
        rm -rf "$TRACE_DIR" "$LOG"
        continue
    fi
    echo "  samples in window: $SAMP (>= $SAMP_MIN required for the derived bound)"
    rate_assertions[$RATE]=$(tiered_arm_assertions "$OUT" "$XVAL_RC" "$TOLERANCE") || rate_assertions[$RATE]=INVALID

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
# Collect EVERY requested rate: a daemon/startup or capture-validity failure
# leaves its assertions unset, so INVALID makes the final decision exit 1.
# This preserves #309's guard against passing by omission.
ARM_ASSERTIONS=()
DEVIATIONS=()
for RATE in $RATES; do
    print_rate_summary "$RATE" "${rate_result[$RATE]:-}" "${rate_delta[$RATE]:-?}" \
        "${rate_event[$RATE]:-n/a}" "$TOLERANCE"
    ARM_ASSERTIONS+=("${rate_assertions[$RATE]:-INVALID}")
    DEVIATIONS+=("${RATE}=${rate_delta[$RATE]:-?}")
done
echo "TIERED_DEVIATIONS ${DEVIATIONS[*]}"

if [[ -n "$best_rate" ]]; then
    echo "First rate within +/-${TOLERANCE}pp tolerance: ${best_rate} Hz"
fi

RESULT_RC=0
tiered_classify "$non_tolerance_failure" "$TOLERANCE" ${ARM_ASSERTIONS[@]+"${ARM_ASSERTIONS[@]}"} || RESULT_RC=$?
if [[ "$RESULT_RC" -eq 0 ]]; then
    echo "PASS: every requested rate ($RATES) is within +/-${TOLERANCE}pp tolerance."
    exit 0
else
    echo "FAIL: not every requested rate is within +/-${TOLERANCE}pp tolerance — see per-rate lines above for which rate(s) and by how much."
    if [[ "$RESULT_RC" -eq 79 ]]; then
        echo "#675 signature only: sampled under-reads exact, all deviations <=15.0 pp (exit 79)."
        exit 79
    fi
    exit 1
fi
