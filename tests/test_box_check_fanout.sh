#!/usr/bin/env bash
# test_box_check_fanout.sh -- unit tests for scripts/box-check-fanout-lib.sh,
# the pure PG-version-list parsing + combined-summary-formatting helpers
# behind `make box-check EPHEMERAL=1 FANOUT=1` (issue: fan the ephemeral live
# tier out across one machine per requested PostgreSQL major instead of
# running them one after another on a single machine).
#
# No network, no Hetzner token, no VM ever created or touched: every case
# here calls the sourced library functions directly (same "pure builder"
# split test_hetzner_sweep.sh uses for the sweep's own decision logic, and
# tests/web_unit/*.test.mjs uses for UI builders). The orchestration itself
# (parallel launch, signal forwarding, actual VM lifecycle) is deliberately
# NOT covered here -- see scripts/box-check.sh's own header comment for why
# that is proven live instead (issue #141 established the same split: this
# script's trap/cleanup logic has never had an automated test either, only
# tests/hetzner-sweep.sh's fallback-janitor decision logic does).
#
# Usage: tests/test_box_check_fanout.sh (run from anywhere; resolves its own dir)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../scripts/box-check-fanout-lib.sh
source "$SCRIPT_DIR/../scripts/box-check-fanout-lib.sh"

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

check_eq() {
    local actual="$1" expected="$2" msg="$3"
    if [[ "$actual" == "$expected" ]]; then
        report true "$msg"
    else
        report false "$msg (got '$actual', want '$expected')"
    fi
}

# ── fanout_parse_versions ────────────────────────────────────────────────

check_eq "$(fanout_parse_versions '')" "13 16 17 18" \
    "empty PG= falls back to the default four-major set"

check_eq "$(fanout_parse_versions '13')" "13" \
    "a single major (no comma) fans out over just that one machine"

check_eq "$(fanout_parse_versions '13,16,17,18')" "13 16 17 18" \
    "a comma-separated list is split into the exact same four majors"

check_eq "$(fanout_parse_versions '13,17')" "13 17" \
    "a partial list (2 of 4 majors) is honoured, not padded back to the default"

check_eq "$(fanout_parse_versions ' 13 , 17 ')" "13 17" \
    "stray whitespace around a comma-separated list is trimmed"

check_eq "$(fanout_parse_versions '13,,17')" "13 17" \
    "a doubled comma (blank entry) is dropped rather than producing an empty version token"

# ── fanout_status_line ───────────────────────────────────────────────────

check_eq "$(fanout_status_line 13 0)" "PG13: PASS (exit=0)" \
    "exit 0 renders as PASS with the exit code shown"

check_eq "$(fanout_status_line 17 1)" "PG17: FAIL (exit=1)" \
    "a non-zero exit renders as FAIL, naming which major failed"

check_eq "$(fanout_status_line 17 2)" "PG17: FAIL (exit=2)" \
    "any non-zero exit code (not just 1) renders as FAIL"

# ── fanout_overall_result ────────────────────────────────────────────────
# THE property that matters most (issue priority #1's sibling for
# attribution: "fails the whole run if any machine fails") -- one red
# machine among four green ones must still fail the whole fanout run. This
# is the exact input ("3 pass, 1 fails") that a broken aggregator (e.g. one
# that only checks the LAST machine, or ORs instead of ANDs) would pass
# while the product (a version that is actually broken) goes unreported.

out=$(fanout_overall_result 0 0 0 0); rc=$?
check_eq "$out" "PASS" "four passing workers -> overall PASS text"
report "$([[ $rc -eq 0 ]] && echo true || echo false)" "four passing workers -> overall exit 0"

out=$(fanout_overall_result 0 0 1 0); rc=$?
check_eq "$out" "FAIL" "one failing worker among four (PG version #3) -> overall FAIL text"
report "$([[ $rc -ne 0 ]] && echo true || echo false)" "one failing worker among four -> overall exit non-zero"

out=$(fanout_overall_result 1 0 0 0); rc=$?
check_eq "$out" "FAIL" "the FIRST worker failing (not just a later one) still fails the whole run"
report "$([[ $rc -ne 0 ]] && echo true || echo false)" "first worker failing -> overall exit non-zero"

out=$(fanout_overall_result); rc=$?
check_eq "$out" "FAIL" "zero workers at all (nothing ran) is a FAIL, never a vacuous PASS"
report "$([[ $rc -ne 0 ]] && echo true || echo false)" "no workers -> overall exit non-zero"

echo
echo "test_box_check_fanout: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
