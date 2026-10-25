# run_all_tiers.sh -- required-tier accounting for tests/run_all.sh (issue #528)
#
# THE DEFECT THIS EXISTS FOR. tests/run_all.sh wrapped its 26-test
# synthetic-data tier in `if [[ -x pgwt-server && -x gen_test_traces ]]`, with
# a single skip_test() in the else branch, and its only refusal was "nothing
# at all executed". So a build where those two binaries are missing produced
# `Executed: 125 (passed 125, failed 0)` and exit 0 with an entire tier --
# including every gate that lives in it -- silently absent. The summary counts
# what ran; a tier-sized hole leaves the count looking healthy. That is the
# "the build broke in a way that removed the tests, and the gate said PASS"
# failure mode, and it is the same family as the two incidents of 2026-10-24
# (a 377-line suite registered in scripts/check.sh but not unit_tests.list, so
# it never ran on a gate box; DIAG_DATA read on the box but never forwarded by
# the launcher).
#
# THE DISTINCTION THIS PRESERVES. A deliberate opt-out (PGWT_RUN_WEB_UI,
# PGWT_RUN_SNAPSHOTS unset) is a judgement an operator made and a reviewer can
# weigh: it stays a plain skip_test(). What must fail is a tier that was
# supposed to run and did not -- an unmet build precondition, a vanished list
# file, or an opt-in whose dependencies are absent (the operator asked for it
# and got nothing).
#
# WHY THESE FUNCTIONS REFUSE RATHER THAN APPROVE. Every argument path that
# cannot be evaluated -- a non-numeric count, a zero count, a missing list
# file, the wrong number of arguments -- raises a tier error, exactly like a
# detected skip does. A guard that cannot see must refuse, never approve: the
# ways this class of check has gone false-negative in this repo are "the
# declared size was 0 so 0 == 0 passed" and "the file was absent so the
# derived count was 0 and nothing looked missing". Both are closed here, and
# tests/test_run_all_tier_skip.sh has a case per bypass.
#
# Sourced by tests/run_all.sh; self-tested by tests/test_run_all_tier_skip.sh.

TIER_ERRORS=0
TIER_ERROR_LINES=()

# tier_error LINE -- record one required-tier problem. Every refusal path in
# this file funnels through here, so the counter and the human-readable reason
# list can never disagree about whether the run should fail.
tier_error() {
    TIER_ERRORS=$((TIER_ERRORS + 1))
    TIER_ERROR_LINES+=("$1")
}

# skip_required_tier NAME COUNT REASON -- a tier that was supposed to run and
# did not. This is NOT a skip: it fails the run, and it names the tier and how
# many tests went missing so the reason is read off the summary instead of
# inferred from a count that happens to be lower than last time.
skip_required_tier() {
    if [[ $# -ne 3 ]]; then
        tier_error "skip_required_tier called with $# argument(s), expected 3 (NAME COUNT REASON) -- refusing"
        return 1
    fi
    local name="$1" count="$2" reason="$3"
    echo ""
    echo "════════════════════════════════════════"
    if [[ "$count" =~ ^[0-9]+$ ]] && [[ "$count" -gt 0 ]]; then
        echo "  $name — REQUIRED TIER DID NOT RUN"
        echo "  $count test(s) never executed: $reason"
        tier_error "$name: $count test(s) never ran — $reason"
    else
        # An unparseable or zero count must not read as "nothing was missing".
        echo "  $name — REQUIRED TIER DID NOT RUN"
        echo "  test count is not a positive integer ('$count'): $reason"
        tier_error "$name: tier skipped AND its declared test count ('$count') is not a positive integer — $reason"
    fi
    echo "  This FAILS the run (issue #528): a required tier that does not"
    echo "  execute is not a pass."
    echo "════════════════════════════════════════"
    return 1
}

# tier_size_check NAME DECLARED BEFORE AFTER -- assert that a tier which DID
# run contributed exactly the number of tests it declares.
#
# Why this is here at all: the count skip_required_tier() reports is a literal
# in run_all.sh (the tier's membership cannot be derived from its own source
# without a parser -- and scripts/check-runner-registration.py reads those
# same literal `python3 "$SCRIPT_DIR/x.py"` lines to decide what is
# registered, so turning the tier into a loop over an array would make 26 real
# test files look like orphans). A literal drifts. This pins it against a
# runtime count taken from the runner's own pass/fail counters, so the two
# sides have different provenance: adding a test to the tier without updating
# the literal fails the very next run, instead of quietly shrinking the number
# a future skip would report.
tier_size_check() {
    if [[ $# -ne 4 ]]; then
        tier_error "tier_size_check called with $# argument(s), expected 4 (NAME DECLARED BEFORE AFTER) -- refusing"
        return 1
    fi
    local name="$1" declared="$2" before="$3" after="$4"
    local v
    for v in "$declared" "$before" "$after"; do
        if [[ ! "$v" =~ ^-?[0-9]+$ ]]; then
            tier_error "$name: tier-size check got a non-numeric value ('$v') — refusing (it cannot tell whether the tier ran)"
            return 1
        fi
    done
    if [[ "$declared" -le 0 ]]; then
        tier_error "$name: declared tier size is '$declared' — refusing (a tier that claims to contain no tests cannot report what skipping it costs)"
        return 1
    fi
    local actual=$((after - before))
    if [[ "$actual" -ne "$declared" ]]; then
        tier_error "$name: declares $declared test(s) but $actual actually ran — refusing (the declared size is what a tier skip reports; update it in tests/run_all.sh in the same commit as the tier change)"
        return 1
    fi
    return 0
}

# tier_list_count FILE -- number of entries tests/run_all.sh's Step 2 loop
# would execute from a list file.
#
# The predicate must be the loop's EXACTLY, or this turns the gate red for a
# reason that has nothing to do with #528 and gets loosened. That loop is
# `while read -r t` + `case "$t" in ''|\#*) continue`, and `read` with the
# default IFS strips leading and trailing whitespace before the case sees it --
# so a whitespace-only line and an INDENTED comment are both skipped by the
# loop. A plain `grep -cvE '^(#|$)'` counts both: on the fixture
# `a / "   " / "  # c" / "\tb\t" / "#c"` the loop executes 2 and that grep
# says 4. Hence the strip-then-match below.
#
# Prints the count on stdout and returns 0. Returns 1 WITH NO OUTPUT when the
# file is missing, unreadable, or holds no entries at all. "Absent" must never
# reach the caller as the number 0: a vanished unit_tests.list would then
# satisfy a 0-expected/0-ran comparison and approve itself, which is the exact
# shape of the hole this file exists to close.
tier_list_count() {
    [[ $# -eq 1 ]] || return 1
    local f="$1" n
    [[ -r "$f" ]] || return 1
    n=$(sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$f" 2>/dev/null \
        | grep -c -v -E '^(#|$)')
    [[ "$n" =~ ^[0-9]+$ ]] || return 1
    [[ "$n" -gt 0 ]] || return 1
    printf '%s\n' "$n"
    return 0
}

# tier_report -- the required-tier section of the SUMMARY. Prints nothing when
# clean, so a green run's tail is unchanged.
tier_report() {
    [[ "$TIER_ERRORS" -eq 0 ]] && return 0
    echo "  Tiers:    $TIER_ERRORS required-tier problem(s) — counted as FAILURES, not skips:"
    local l
    for l in ${TIER_ERROR_LINES[@]+"${TIER_ERROR_LINES[@]}"}; do
        echo "            - $l"
    done
    return 0
}

# tier_ok -- 0 when no required tier is missing. tests/run_all.sh's exit
# status is gated on this in addition to its failed count.
tier_ok() {
    [[ "$TIER_ERRORS" -eq 0 ]]
}
