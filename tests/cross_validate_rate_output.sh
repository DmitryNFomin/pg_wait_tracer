#!/usr/bin/env bash
# Labels for the per-rate cross-validation output.
#
# These lines are NOT informational. test_cross_validate_tiered.sh exits 1 --
# and run_all.sh counts the test as failed -- when no swept rate lands inside
# the tolerance, so an "outside tolerance" line is part of a failing test.
# It said "(characterisation, non-gating)" on BOTH paths until 2026-10-07,
# which is why a red test read as a report for over a week. The wording below
# states, per path, what the line does to the test's result.

rate_label() {
    if [[ "$1" == 10 ]]; then
        echo "shipped default"
    else
        echo "exploratory rate"
    fi
}

print_rate_result() {
    local rate="$1" output="$2" label
    label=$(rate_label "$rate")
    printf '%s\n' "$output" | sed \
        "s/^RESULT: PASS$/RATE ${rate}Hz (${label}): within tolerance (rate characterisation; this rate satisfies the test)/; s/^RESULT: FAIL$/RATE ${rate}Hz (${label}): outside tolerance (rate characterisation; FAILS the test unless another rate is within tolerance)/; s/^/  /"
}

print_rate_summary() {
    local rate="$1" result="$2" verdict label
    label=$(rate_label "$rate")
    case "$result" in
        PASS) verdict="within tolerance (rate characterisation; this rate satisfies the test)" ;;
        FAIL) verdict="outside tolerance (rate characterisation; FAILS the test unless another rate is within tolerance)" ;;
        *) verdict="no result (rate characterisation; the rate produced no verdict -- FAILS the test unless another rate is within tolerance)" ;;
    esac
    printf '  RATE %sHz (%s): %s\n' "$rate" "$label" "$verdict"
}
