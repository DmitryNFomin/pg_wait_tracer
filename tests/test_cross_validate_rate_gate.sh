#!/usr/bin/env bash
# test_cross_validate_rate_gate.sh -- unit test for the pure gating logic in
# tests/cross_validate_rate_output.sh (issue #309).
#
# test_cross_validate_tiered.sh itself needs root, a live postgres, pgbench
# and a built BPF daemon, so it can only run on the Linux gate box. The
# DECISION it makes -- "did every requested sample rate land within
# tolerance" -- and the parsing that feeds it are pure bash with no live
# dependency, so they are pulled out and tested here, on the Mac, via
# `make check` (same pattern as test_classify_changed_files.sh).
#
# Pure bash, no git, no network, no Linux-only dependency.
# Usage: bash tests/test_cross_validate_rate_gate.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/cross_validate_rate_output.sh"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

# old_vacuous_gate TOKEN...
# The PRE-#309 decision, kept here ONLY so the tests below can show the bug
# this file guards against: it passes as soon as ANY ONE token is PASS,
# which is exactly how a run with 3 of 4 rates out of tolerance printed
# overall success. It is not sourced from product code -- it never shipped
# as a function, it was inline in the exit-code check -- it is reproduced
# here as the historical behaviour under test.
old_vacuous_gate() {
    local tok
    for tok in "$@"; do
        [[ "$tok" == "PASS" ]] && return 0
    done
    return 1
}

# --- all_passed: the new gate ----------------------------------------------

if all_passed PASS PASS PASS PASS; then ok=true; else ok=false; fi
report "$ok" "all_passed: 4/4 PASS -> true"

# THE #309 defect scenario: 3 of 4 rates out of tolerance. The input that
# makes the OLD gate wrongly pass, and the NEW gate correctly fail.
if all_passed FAIL FAIL FAIL PASS; then ok=false; else ok=true; fi
report "$ok" "all_passed: 1/4 PASS, 3/4 FAIL -> false (the #309 scenario)"

if old_vacuous_gate FAIL FAIL FAIL PASS; then old_ok=true; else old_ok=false; fi
report "$old_ok" "old_vacuous_gate: same input -> true (demonstrates the bug #309 fixes)"

# Any single rate out of tolerance fails, not just "most of them".
if all_passed PASS PASS PASS FAIL; then ok=false; else ok=true; fi
report "$ok" "all_passed: 3/4 PASS, 1/4 FAIL (the last one) -> false"

# Empty input (e.g. --rates "" or a caller that forgot to pass any results)
# must not vacuously pass -- absence of evidence is not evidence of a pass.
if all_passed; then ok=false; else ok=true; fi
report "$ok" "all_passed: no tokens at all -> false (empty input is not a pass)"

# A rate whose daemon never started has no PASS/FAIL verdict at all -- an
# empty string, not "FAIL" -- and must still fail the gate rather than be
# silently skipped (the test driver defaults a missing rate_result entry to
# "" for exactly this reason).
if all_passed PASS "" PASS PASS; then ok=false; else ok=true; fi
report "$ok" "all_passed: one rate never measured (empty token) -> false, not skipped"

# --- extract_max_delta: parsing cross_validate's own output ----------------

SAMPLE_PASS_OUT=$'some preamble\nMax share disagreement (events >= 2% exact share): 3.4 pp (LWLock)\nTolerance: +/- 10.0 pp\n\nRESULT: PASS'
read -r delta event <<< "$(extract_max_delta "$SAMPLE_PASS_OUT" | tr '|' ' ')"
if [[ "$delta" == "3.4" && "$event" == "LWLock" ]]; then ok=true; else ok=false; fi
report "$ok" "extract_max_delta: parses delta+event from a real PASS block (got '$delta'/'$event')"

SAMPLE_FAIL_OUT=$'Max share disagreement (events >= 2% exact share): 14.9 pp (BufferPin)\nTolerance: +/- 10.0 pp\n\nRESULT: FAIL'
read -r delta event <<< "$(extract_max_delta "$SAMPLE_FAIL_OUT" | tr '|' ' ')"
if [[ "$delta" == "14.9" && "$event" == "BufferPin" ]]; then ok=true; else ok=false; fi
report "$ok" "extract_max_delta: parses delta+event from a real FAIL block (got '$delta'/'$event')"

# Unparseable / missing field: the tool errored before printing the line, or
# was handed empty output. Must read as unknown, never as a silent zero
# delta (which would look like a pass).
read -r delta event <<< "$(extract_max_delta "" | tr '|' ' ')"
if [[ "$delta" == "?" && "$event" == "n/a" ]]; then ok=true; else ok=false; fi
report "$ok" "extract_max_delta: empty input -> '?'/'n/a', not a fabricated zero (got '$delta'/'$event')"

read -r delta event <<< "$(extract_max_delta "RESULT: FAIL" | tr '|' ' ')"
if [[ "$delta" == "?" && "$event" == "n/a" ]]; then ok=true; else ok=false; fi
report "$ok" "extract_max_delta: output missing the disagreement line -> '?'/'n/a' (got '$delta'/'$event')"

# --- the label contradiction: must actually be gone -------------------------

PASS_LINE=$(print_rate_result 10 "$SAMPLE_PASS_OUT")
FAIL_LINE=$(print_rate_result 50 "$SAMPLE_FAIL_OUT")
if [[ "$PASS_LINE" != *"non-gating"* && "$FAIL_LINE" != *"non-gating"* ]]; then ok=true; else ok=false; fi
report "$ok" "print_rate_result: 'non-gating' label is gone from both the pass and fail path"

SUMMARY_FAIL=$(print_rate_summary 50 FAIL 14.9 BufferPin 10)
if [[ "$SUMMARY_FAIL" == *"50"* && "$SUMMARY_FAIL" == *"OUTSIDE TOLERANCE"* \
      && "$SUMMARY_FAIL" == *"14.9"* && "$SUMMARY_FAIL" == *"BufferPin"* \
      && "$SUMMARY_FAIL" == *"10"* ]]; then
    ok=true
else
    ok=false
fi
report "$ok" "print_rate_summary: FAIL line names the rate, the delta, the event and the tolerance (got: $SUMMARY_FAIL)"

SUMMARY_NORESULT=$(print_rate_summary 200 "" "?" "n/a" 10)
if [[ "$SUMMARY_NORESULT" == *"NO RESULT"* ]]; then ok=true; else ok=false; fi
report "$ok" "print_rate_summary: a never-measured rate prints NO RESULT, not a silent PASS"

echo
echo "cross_validate_rate_gate: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
