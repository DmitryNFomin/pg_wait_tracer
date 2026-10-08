#!/usr/bin/env bash
# Labels and per-rate gating logic for cross-validation output.
#
# Issue #309: every line here used to end "(characterisation, non-gating)" on
# BOTH the pass and the fail path, even though test_cross_validate_tiered.sh's
# exit code already depended on these results (it failed the run when NO
# rate passed). The label was false on the fail path already, and after the
# #309 fix below -- every requested rate must pass, not just one -- it would
# be false on the pass path too: there is no non-gating mode of this script,
# so the label is removed rather than kept-and-wrong.

rate_label() {
    if [[ "$1" == 10 ]]; then
        echo "shipped default"
    else
        echo "exploratory rate"
    fi
}

# extract_max_delta OUTPUT
# Echoes "DELTA_PP|EVENT_NAME" parsed out of cross_validate's
#   "Max share disagreement (events >= N% exact share): D pp (EVENT)"
# line. Echoes "?|n/a" when that line is missing (tool errored before
# printing it, or OUTPUT is empty) -- a parse failure must read as
# "unknown", never silently as zero delta (which would look like a pass).
extract_max_delta() {
    local output="$1" line delta event
    line=$(printf '%s\n' "$output" | grep '^Max share disagreement' | tail -1)
    if [[ -z "$line" ]]; then
        echo "?|n/a"
        return
    fi
    delta=$(printf '%s\n' "$line" | sed -n 's/.*: \([0-9.][0-9.]*\) pp (.*/\1/p')
    event=$(printf '%s\n' "$line" | sed -n 's/.*pp (\(.*\))[[:space:]]*$/\1/p')
    [[ -z "$delta" ]] && delta="?"
    [[ -z "$event" ]] && event="n/a"
    echo "${delta}|${event}"
}

print_rate_result() {
    local rate="$1" output="$2" label delta_info delta event
    label=$(rate_label "$rate")
    delta_info=$(extract_max_delta "$output")
    delta="${delta_info%%|*}"
    event="${delta_info#*|}"
    if [[ "$output" == *"RESULT: PASS"* ]]; then
        printf '  RATE %sHz (%s): within tolerance (max disagreement %s pp, %s)\n' \
            "$rate" "$label" "$delta" "$event"
    else
        printf '  RATE %sHz (%s): OUTSIDE TOLERANCE -- max disagreement %s pp (%s)\n' \
            "$rate" "$label" "$delta" "$event"
    fi
}

# print_rate_summary RATE RESULT DELTA EVENT TOLERANCE
# RESULT is "PASS", "FAIL", or "" (no result -- e.g. the daemon never came up
# for this rate). "" prints distinctly from FAIL, but all_passed below
# treats it exactly like FAIL: a rate that was never measured is not
# evidence it was within tolerance, so it cannot pass the gate by omission.
print_rate_summary() {
    local rate="$1" result="$2" delta="$3" event="$4" tolerance="$5" label
    label=$(rate_label "$rate")
    case "$result" in
        PASS) printf '  RATE %sHz (%s): within tolerance (max disagreement %s pp, %s)\n' \
                "$rate" "$label" "$delta" "$event" ;;
        FAIL) printf '  RATE %sHz (%s): OUTSIDE TOLERANCE -- max disagreement %s pp (%s) vs +/-%s pp tolerance\n' \
                "$rate" "$label" "$delta" "$event" "$tolerance" ;;
        *) printf '  RATE %sHz (%s): NO RESULT -- daemon/workload never completed for this rate (counts as a failure, not a skip)\n' \
                "$rate" "$label" ;;
    esac
}

# all_passed TOKEN...
# TOKENs are the per-rate results, each exactly "PASS" or anything else
# ("FAIL", "", unset-and-defaulted-to-""). True (exit 0) only if EVERY token
# is PASS and at least one token was given. Issue #309: the old gate exited
# 0 as soon as ANY ONE rate passed, so a run where 3 of 4 rates were outside
# tolerance still printed overall success.
all_passed() {
    local tok seen=0
    for tok in "$@"; do
        seen=1
        [[ "$tok" == "PASS" ]] || return 1
    done
    [[ $seen -eq 1 ]]
}
