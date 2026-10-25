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
# out-of-tolerance rate (or one that could not even be measured, e.g. the
# daemon never starting) fails the run. The first rate that PASSES is still
# printed as the recommended default -- that framing is unchanged -- but it
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
    print_rate_result "$RATE" "$OUT"
    DELTA_INFO=$(extract_max_delta "$OUT")
    rate_delta[$RATE]="${DELTA_INFO%%|*}"
    rate_event[$RATE]="${DELTA_INFO#*|}"

    # Did the comparator see enough of a capture for the derived bound to
    # mean anything? Two distinct refusals, both leaving rate_result UNSET
    # so the summary prints NO RESULT and all_passed counts it as a failure
    # (the #309 pass-by-omission rule):
    #
    #  1. N absent or unparseable -- the gate is blind. A blind gate must
    #     refuse, never approve; this is the same disposition
    #     extract_max_delta already takes for a missing delta line.
    #  2. at the derived rate (10 Hz) only, N below the 3130 the bound's
    #     sampling error was computed at. That floor is not a guess: the
    #     bound budgets 2.2958 pp of its 8.8 to noise at z(5)=2.5688, which
    #     is exactly SE <= 0.8937 pp, which is exactly N >= 0.25*100^2 /
    #     0.8937^2 = 3130. Below it the capture's SE exceeds what the bound
    #     allocated, so a "pass" would certify a looser quantity than the
    #     one derived. Other rates get only refusal (1), because the
    #     derivation covers 10 Hz and the observed N is measurably
    #     sub-linear in rate (33,778 at 100 Hz against 31,300 scaled), so a
    #     scaled floor would be invention, not arithmetic.
    SAMP=$(extract_samples_in_window "$OUT")
    SAMP_MIN=1
    [[ "$RATE" == 10 ]] && SAMP_MIN="$XVAL_PREREG_SAMPLES"
    if ! refuse_unless_samples_sufficient "$SAMP" "$SAMP_MIN" >/dev/null; then
        echo "  RATE ${RATE}Hz: REFUSED — capture too thin / sample count unreadable" \
             "(N=${SAMP}, need >=${SAMP_MIN}); counts as a failure, not a skip"
        rm -rf "$TRACE_DIR" "$LOG"
        continue
    fi
    echo "  samples in window: $SAMP (>= $SAMP_MIN required for the derived bound)"

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
# Collect each requested rate's result as a token for all_passed, in the
# SAME iteration order used to print the table -- a rate whose daemon never
# started has no rate_result entry at all, so it defaults to "" here, which
# all_passed treats as a failure rather than silently skipping it (issue
# #309 bypass case: a component failing to even run must not read as a
# pass-by-omission).
RESULT_TOKENS=()
for RATE in $RATES; do
    print_rate_summary "$RATE" "${rate_result[$RATE]:-}" "${rate_delta[$RATE]:-?}" \
        "${rate_event[$RATE]:-n/a}" "$TOLERANCE"
    RESULT_TOKENS+=("${rate_result[$RATE]:-}")
done

if [[ -n "$best_rate" ]]; then
    echo "First rate within +/-${TOLERANCE}pp tolerance: ${best_rate} Hz"
fi

if all_passed ${RESULT_TOKENS[@]+"${RESULT_TOKENS[@]}"}; then
    echo "PASS: every requested rate ($RATES) is within +/-${TOLERANCE}pp tolerance."
    exit 0
else
    echo "FAIL: not every requested rate is within +/-${TOLERANCE}pp tolerance — see per-rate lines above for which rate(s) and by how much."
    exit 1
fi
