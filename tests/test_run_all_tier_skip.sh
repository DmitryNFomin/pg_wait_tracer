#!/usr/bin/env bash
# test_run_all_tier_skip.sh -- issue #528: a skipped REQUIRED tier must fail
# tests/run_all.sh, and the reason must be readable off the summary instead of
# inferred from a test count that happens to be lower than last time.
#
# Two halves:
#
#   PART A  unit cases for tests/run_all_tiers.sh's functions, sourced
#           directly. Mostly BYPASS cases: every way this guard could be made
#           unable to see (zero count, non-numeric count, missing list file,
#           wrong argument count, a tier that is absent rather than wrong) must
#           REFUSE, never approve. The repo's recurring defect in this class is
#           the false negative -- a conservation check whose two sides came
#           from the same sum, a guard that approved when it could not resolve
#           a base -- not the true positive.
#
#   PART B  end-to-end runs of the REAL tests/run_all.sh against throwaway
#           fixtures under mktemp -d. B1 is the anti-vacuity control: the same
#           fixture with the tier's binaries present must exit 0, otherwise
#           every red below proves only that the fixture is broken. B5 is the
#           negative control the issue asks for: the pre-#528 wrapping
#           (skip_test instead of skip_required_tier) restored by a verified
#           mutation of the fixture's copy, which must go GREEN on exactly the
#           scenario the shipped version refuses -- that is what shows the hole
#           was real and that the refusal is the thing closing it.
#
# The fixture makes the live tier deterministic on ANY machine by putting a
# `pgrep` that always exits 1 at the front of PATH: run_all.sh's live section
# is gated on `id -u == 0 && pgrep -x postgres`, so this takes the skip branch
# whether the test runs as a user on a Mac or as root on a gate box with four
# clusters up. Without it, this test's expected output would depend on who ran
# it, and the first environment to disagree would get the assertion loosened.
#
# Usage: bash tests/test_run_all_tier_skip.sh   (no root, no PG, no network)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0

ok() { echo "  ok   -- $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL -- $1"; fail=$((fail + 1)); }

check() {  # check DESC EXPECTED ACTUAL
    if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}

# ─────────────────────────────── PART A ───────────────────────────────
echo "PART A -- tests/run_all_tiers.sh functions"
source "$SCRIPT_DIR/run_all_tiers.sh"

reset_tiers() { TIER_ERRORS=0; TIER_ERROR_LINES=(); }

# A1: a clean run reports nothing and passes. If this were not true the whole
# guard would be a permanent red and would be removed within a day.
reset_tiers
check "A1 clean: tier_ok succeeds" "0" "$(tier_ok; echo $?)"
check "A1 clean: tier_report prints nothing" "" "$(tier_report)"

# A2: TRUE POSITIVE -- a required tier skip fails and names tier + count.
#
# NOTE on $(...): command substitution is a subshell, so a counter incremented
# inside one never reaches this script. Every call whose SIDE EFFECT is being
# asserted therefore redirects to a file; only read-only calls (tier_ok,
# tier_report) are captured with $(...).
A_OUT="$(mktemp "${TMPDIR:-/tmp}/tierout.XXXXXX")"
reset_tiers
skip_required_tier "synthetic tier" 26 "pgwt-server not built" > "$A_OUT"
check "A2 skip_required_tier returns non-zero" "1" "$(reset_tiers; skip_required_tier x 1 y >/dev/null; echo $?)"
reset_tiers
skip_required_tier "synthetic tier" 26 "pgwt-server not built" > "$A_OUT"
a2_out=$(cat "$A_OUT")
check "A2 TIER_ERRORS incremented" "1" "$TIER_ERRORS"
check "A2 tier_ok now fails" "1" "$(tier_ok; echo $?)"
case "$a2_out" in
    *"synthetic tier"*"26 test(s) never executed"*"pgwt-server not built"*)
        ok "A2 block names the tier, the count and the reason" ;;
    *) bad "A2 block text: $a2_out" ;;
esac
case "$(tier_report)" in
    *"synthetic tier: 26 test(s) never ran"*) ok "A2 summary line carries tier + count" ;;
    *) bad "A2 tier_report: $(tier_report)" ;;
esac

# ── BYPASS A: a count of 0. "0 tests are missing" must not read as "nothing
# is missing" -- otherwise a future edit that lets the declared size drift to
# zero silently disarms the refusal.
reset_tiers
skip_required_tier "zero tier" 0 "binaries absent" > "$A_OUT"
b_out=$(cat "$A_OUT")
check "BYPASS-A zero count still refuses" "1" "$TIER_ERRORS"
case "$b_out" in
    *"not a positive integer"*) ok "BYPASS-A says why the count is unusable" ;;
    *) bad "BYPASS-A text: $b_out" ;;
esac

# ── BYPASS B: an unparseable count (the "unknown" run_all.sh passes when it
# cannot resolve a list) must refuse, not be treated as absence-of-problem.
for badcount in "unknown" "" "26x" "-3"; do
    reset_tiers
    skip_required_tier "weird tier" "$badcount" "reason" >/dev/null
    check "BYPASS-B count '$badcount' still refuses" "1" "$TIER_ERRORS"
done

# ── BYPASS C: the wrong number of arguments. A caller refactored into passing
# 2 args must not silently succeed.
for n in 0 1 2 4; do
    reset_tiers
    case "$n" in
        0) skip_required_tier >/dev/null 2>&1 ;;
        1) skip_required_tier a >/dev/null 2>&1 ;;
        2) skip_required_tier a 1 >/dev/null 2>&1 ;;
        4) skip_required_tier a 1 b c >/dev/null 2>&1 ;;
    esac
    check "BYPASS-C skip_required_tier with $n args refuses" "1" "$TIER_ERRORS"
done
for n in 0 3 5; do
    reset_tiers
    case "$n" in
        0) tier_size_check >/dev/null 2>&1 ;;
        3) tier_size_check a 1 0 >/dev/null 2>&1 ;;
        5) tier_size_check a 1 0 1 extra >/dev/null 2>&1 ;;
    esac
    check "BYPASS-C tier_size_check with $n args refuses" "1" "$TIER_ERRORS"
done

# A3: tier_size_check -- the declared size pinned against what actually ran.
reset_tiers
tier_size_check "t" 26 100 126
check "A3 declared 26, ran 26 -> clean" "0" "$TIER_ERRORS"
reset_tiers
tier_size_check "t" 26 100 125
check "A3 declared 26, ran 25 (PARTIAL range) -> refuses" "1" "$TIER_ERRORS"
case "$(tier_report)" in
    *"declares 26 test(s) but 25 actually ran"*) ok "A3 partial-range message" ;;
    *) bad "A3 partial-range message: $(tier_report)" ;;
esac
reset_tiers
tier_size_check "t" 26 100 127
check "A3 declared 26, ran 27 (tier GREW) -> refuses" "1" "$TIER_ERRORS"
reset_tiers
tier_size_check "t" 26 100 99
check "A3 counters went backwards -> refuses" "1" "$TIER_ERRORS"

# ── BYPASS D: tier_size_check's two sides must have different provenance. A
# declared size of 0 compared against a delta of 0 is the "conservation check
# whose two sides came from the same sum" shape: it can never fail. Refuse.
reset_tiers
tier_size_check "t" 0 100 100
check "BYPASS-D declared 0 vs ran 0 refuses (cannot-fail comparison)" "1" "$TIER_ERRORS"
reset_tiers
tier_size_check "t" "" 100 100
check "BYPASS-D empty declared size refuses" "1" "$TIER_ERRORS"
reset_tiers
tier_size_check "t" 26 "" 126
check "BYPASS-D unparseable BEFORE refuses" "1" "$TIER_ERRORS"
reset_tiers
tier_size_check "t" 26 100 "abc"
check "BYPASS-D unparseable AFTER refuses" "1" "$TIER_ERRORS"

# A4 / ── BYPASS E: tier_list_count. The list being ABSENT (rather than
# wrong) is the case that must not come back as the number 0 -- a vanished
# unit_tests.list would then satisfy "0 expected, 0 ran".
tmp_list=$(mktemp -d "${TMPDIR:-/tmp}/tierlist.XXXXXX")
trap 'rm -rf "$tmp_list" "$A_OUT"' EXIT
printf 'a\n# comment\n\nb\nc\n' > "$tmp_list/good.list"
check "A4 counts entries, ignoring blanks and comments" "3" "$(tier_list_count "$tmp_list/good.list")"

# A4b: the predicate must match Step 2's loop on the cases where a naive
# `grep -cvE '^(#|$)'` does NOT -- an INDENTED comment and a whitespace-only
# line, both of which `read -r` + `case` skip (read strips surrounding IFS
# whitespace before the case runs). A disagreement here would refuse a
# perfectly good box run the first time someone indents a comment in
# unit_tests.list, and a gate that goes red for the wrong reason gets
# loosened. Driven against the loop itself, not against a second hardcoded
# number.
printf 'a\n   \n  # indented\n\tb\t\n#c\n' > "$tmp_list/ws.list"
ws_loop=0
while read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    ws_loop=$((ws_loop + 1))
done < "$tmp_list/ws.list"
check "A4b whitespace fixture: tier_list_count == Step 2's loop" \
    "$ws_loop" "$(tier_list_count "$tmp_list/ws.list")"
check "A4b (and that loop count is the expected 2, not a tautology)" "2" "$ws_loop"
check "A4b a naive grep would have said 4 here" "4" \
    "$(grep -c -v -E '^(#|$)' "$tmp_list/ws.list")"
printf '   \n\t\n  # all blank or indented comment\n' > "$tmp_list/wsonly.list"
check "A4b whitespace/indented-comment-only file returns non-zero" "1" \
    "$(tier_list_count "$tmp_list/wsonly.list" >/dev/null 2>&1; echo $?)"
check "BYPASS-E missing file prints nothing" "" "$(tier_list_count "$tmp_list/nope.list" 2>/dev/null)"
check "BYPASS-E missing file returns non-zero" "1" "$(tier_list_count "$tmp_list/nope.list" >/dev/null 2>&1; echo $?)"
printf '# only comments\n\n' > "$tmp_list/empty.list"
check "BYPASS-E comments-only file returns non-zero" "1" "$(tier_list_count "$tmp_list/empty.list" >/dev/null 2>&1; echo $?)"
check "BYPASS-E comments-only file prints nothing" "" "$(tier_list_count "$tmp_list/empty.list" 2>/dev/null)"
: > "$tmp_list/zero.list"
check "BYPASS-E zero-byte file returns non-zero" "1" "$(tier_list_count "$tmp_list/zero.list" >/dev/null 2>&1; echo $?)"
if [[ "$(id -u)" -ne 0 ]]; then
    printf 'a\n' > "$tmp_list/noread.list"
    chmod 000 "$tmp_list/noread.list"
    check "BYPASS-E unreadable file returns non-zero" "1" \
        "$(tier_list_count "$tmp_list/noread.list" >/dev/null 2>&1; echo $?)"
    chmod 644 "$tmp_list/noread.list"
else
    # Running as root, mode 000 is still readable; the case cannot be staged,
    # so say so rather than printing a pass nobody earned.
    echo "  note -- BYPASS-E unreadable-file case not stageable as root (mode 000 is readable); covered by the missing-file case above"
fi
check "BYPASS-E no-argument call returns non-zero" "1" "$(tier_list_count >/dev/null 2>&1; echo $?)"

# A5: the real tests/unit_tests.list must be countable by the same predicate
# run_all.sh's Step 2 loop uses -- otherwise the shipped C-tier check would
# refuse on every run and get reverted.
real_count=$(tier_list_count "$SCRIPT_DIR/unit_tests.list" || true)
loop_count=0
while read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    loop_count=$((loop_count + 1))
done < "$SCRIPT_DIR/unit_tests.list"
check "A5 tier_list_count agrees with Step 2's own loop predicate" "$loop_count" "$real_count"
if [[ "${real_count:-0}" -gt 0 ]]; then
    ok "A5 real unit_tests.list yields a positive count ($real_count)"
else
    bad "A5 real unit_tests.list yielded no count"
fi

# ─────────────────────────────── PART B ───────────────────────────────
echo ""
echo "PART B -- end-to-end tests/run_all.sh fixtures"

FIX_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tierfix.XXXXXX")
trap 'rm -rf "$tmp_list" "$A_OUT" "$FIX_ROOT"' EXIT

# build_fixture DIR -- a minimal repo in which the REAL run_all.sh runs green:
# stub Makefiles, a stub for every test file run_all.sh invokes directly
# (found with the same `python3 "$SCRIPT_DIR/x.py"` form
# scripts/check-runner-registration.py reads, so a test added to run_all.sh
# later is stubbed automatically instead of turning B1 red for the wrong
# reason), a two-entry unit_tests.list, and the pgwt-server /
# gen_test_traces binaries the synthetic tier gates on.
build_fixture() {
    local d="$1"
    mkdir -p "$d/tests/web_unit" "$d/bin" "$d/pyshim"
    cp "$SCRIPT_DIR/run_all.sh" "$SCRIPT_DIR/testutil.sh" \
       "$SCRIPT_DIR/run_all_tiers.sh" "$d/tests/" || return 1
    printf 'all:\n\t@true\n' > "$d/Makefile"
    printf 'all:\n\t@true\n' > "$d/tests/Makefile"
    # Force the live section to its skip branch on every machine (see header).
    printf '#!/bin/sh\nexit 1\n' > "$d/bin/pgrep"
    chmod +x "$d/bin/pgrep"
    # Make `import playwright, websockets` fail deterministically even where
    # both are installed, so the opt-in-but-deps-absent branch is reachable.
    printf 'raise ImportError("forced by test_run_all_tier_skip.sh")\n' \
        > "$d/pyshim/websockets.py"
    local inv name
    for inv in $(grep -oE '(python3|bash) "\$SCRIPT_DIR/[A-Za-z0-9_]+\.(py|sh)"' \
                     "$d/tests/run_all.sh" | sed -E 's/.*SCRIPT_DIR\///; s/"$//' | sort -u); do
        name="$inv"
        case "$name" in
            *.py) printf 'print("stub %s")\n' "$name" > "$d/tests/$name" ;;
            *.sh) printf '#!/bin/sh\necho "stub %s"\nexit 0\n' "$name" > "$d/tests/$name" ;;
        esac
        chmod +x "$d/tests/$name"
    done
    printf 'stub_unit_a.py\nstub_unit_b.py\n' > "$d/tests/unit_tests.list"
    printf 'print("stub unit a")\n' > "$d/tests/stub_unit_a.py"
    printf 'print("stub unit b")\n' > "$d/tests/stub_unit_b.py"
    printf 'import { test } from "node:test";\ntest("stub", () => {});\n' \
        > "$d/tests/web_unit/stub.test.mjs"
    : > "$d/pgwt-server"; chmod +x "$d/pgwt-server"
    : > "$d/tests/gen_test_traces"; chmod +x "$d/tests/gen_test_traces"
}

# run_fixture DIR LOGFILE [extra env assignments...] -- prints the exit code.
run_fixture() {
    local d="$1" log="$2"; shift 2
    (
        unset PGWT_RUN_WEB_UI PGWT_RUN_SNAPSHOTS PGWT_PGPORT PGPORT
        export PATH="$d/bin:$PATH"
        env "$@" bash "$d/tests/run_all.sh" > "$log" 2>&1
        echo $?
    )
}

contains() { grep -qF -- "$2" "$1"; }

# assert_case DESC LOG RC EXPECT_RC NEEDLES...
assert_case() {
    local desc="$1" log="$2" rc="$3" want_rc="$4"; shift 4
    local bad_needle=0 n
    for n in "$@"; do
        if ! contains "$log" "$n"; then
            bad "$desc: summary does not mention '$n' (log: $log)"
            bad_needle=1
        fi
    done
    if [[ "$want_rc" == "zero" && "$rc" -ne 0 ]]; then
        bad "$desc: expected exit 0, got $rc (log: $log)"
    elif [[ "$want_rc" == "nonzero" && "$rc" -eq 0 ]]; then
        bad "$desc: expected a non-zero exit, got 0 (log: $log)"
    elif [[ "$bad_needle" -eq 0 ]]; then
        ok "$desc"
    fi
}

# B1 -- THE ANTI-VACUITY CONTROL. Binaries present: the fixture must go green
# and must actually have run the synthetic tier. Every red below is only
# meaningful relative to this.
b1="$FIX_ROOT/b1"; build_fixture "$b1" || bad "B1 fixture build failed"
rc=$(run_fixture "$b1" "$FIX_ROOT/b1.log")
assert_case "B1 GREEN control: tier present -> exit 0" "$FIX_ROOT/b1.log" "$rc" zero \
    "test_data_time_model" "test_protocol_drift" "failed 0"
if contains "$FIX_ROOT/b1.log" "REQUIRED TIER DID NOT RUN"; then
    bad "B1: green control reported a tier skip"
else
    ok "B1: green control reports no tier skip"
fi

# B2 -- the issue's acceptance case: gen_test_traces loses its executable bit.
b2="$FIX_ROOT/b2"; build_fixture "$b2" || bad "B2 fixture build failed"
chmod -x "$b2/tests/gen_test_traces"
rc=$(run_fixture "$b2" "$FIX_ROOT/b2.log")
assert_case "B2 RED: gen_test_traces not executable -> run FAILS, naming tier + count" \
    "$FIX_ROOT/b2.log" "$rc" nonzero \
    "REQUIRED TIER DID NOT RUN" "test_data_* (synthetic-data tier)" \
    "26 test(s) never executed" "refusing to report success"
if contains "$FIX_ROOT/b2.log" "test_data_time_model"; then
    bad "B2: the tier appears to have run anyway"
else
    ok "B2: the tier really did not run (its tests are absent from the log)"
fi

# B3 -- the other half of the precondition: pgwt-server missing entirely.
b3="$FIX_ROOT/b3"; build_fixture "$b3" || bad "B3 fixture build failed"
rm -f "$b3/pgwt-server"
rc=$(run_fixture "$b3" "$FIX_ROOT/b3.log")
assert_case "B3 RED: pgwt-server absent -> run FAILS" "$FIX_ROOT/b3.log" "$rc" nonzero \
    "REQUIRED TIER DID NOT RUN" "26 test(s) never executed"

# B4 -- the pre-#528 arithmetic, reproduced: with the tier skipped, `failed`
# is 0 and `executed` is far from 0, so neither of the runner's old signals
# fires. Assert that explicitly, so a reader can see the refusal is the ONLY
# thing standing between this scenario and a green verdict.
if contains "$FIX_ROOT/b2.log" "failed 0"; then
    ok "B4: with the tier skipped, the old signals still look healthy (failed 0, executed > 0)"
else
    bad "B4: expected 'failed 0' in the tier-skipped run (log: $FIX_ROOT/b2.log)"
fi

# B5 -- NEGATIVE CONTROL. Restore the pre-#528 wrapping in the fixture's own
# copy (skip_required_tier -> skip_test for the synthetic tier) and re-run B2's
# exact scenario. It must go GREEN: that is the defect, demonstrated, and it is
# what makes B2's red attributable to this fix rather than to the fixture.
b5="$FIX_ROOT/b5"; build_fixture "$b5" || bad "B5 fixture build failed"
chmod -x "$b5/tests/gen_test_traces"
python3 - "$b5/tests/run_all.sh" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
# the synthetic tier's refusal -> the plain skip_test it used to be
new = s.replace('skip_required_tier "test_data_* (synthetic-data tier)"',
                'skip_test "test_data_* (synthetic-data tier)"')
if new == s:
    sys.exit("MUTATION DID NOT APPLY")
open(p, "w").write(new)
PYEOF
if [[ $? -ne 0 ]]; then
    bad "B5: could not stage the pre-#528 mutant (mutation did not apply)"
else
    if cmp -s "$b5/tests/run_all.sh" "$SCRIPT_DIR/run_all.sh"; then
        bad "B5: mutant is byte-identical to the shipped runner -- the negative control would be vacuous"
    else
        ok "B5: mutant differs from the shipped runner (mutation verified applied)"
    fi
    rc=$(run_fixture "$b5" "$FIX_ROOT/b5.log")
    assert_case "B5 NEGATIVE CONTROL: pre-#528 wrapping reports PASS on B2's scenario" \
        "$FIX_ROOT/b5.log" "$rc" zero "SKIPPED" "failed 0"
fi

# B6 -- the same hole in the C unit tier: the list file vanishes, the loop body
# runs zero times, and nothing used to notice.
b6="$FIX_ROOT/b6"; build_fixture "$b6" || bad "B6 fixture build failed"
rm -f "$b6/tests/unit_tests.list"
rc=$(run_fixture "$b6" "$FIX_ROOT/b6.log")
assert_case "B6 RED: unit_tests.list absent -> run FAILS, naming the C tier" \
    "$FIX_ROOT/b6.log" "$rc" nonzero \
    "C unit tier (tests/unit_tests.list)" "refusing to report success"

# B7 -- list present but holding no entries: 0 expected vs 0 ran must not pass.
b7="$FIX_ROOT/b7"; build_fixture "$b7" || bad "B7 fixture build failed"
printf '# every line a comment\n\n' > "$b7/tests/unit_tests.list"
rc=$(run_fixture "$b7" "$FIX_ROOT/b7.log")
assert_case "B7 RED: comments-only unit_tests.list -> run FAILS (not 0 == 0)" \
    "$FIX_ROOT/b7.log" "$rc" nonzero "C unit tier (tests/unit_tests.list)"

# B8 -- declared tier size drifts below the real membership: drop one
# run_test line from the synthetic tier so 25 run while 26 are declared. This
# is the check that keeps the count in B2's message honest as tests are added.
b8="$FIX_ROOT/b8"; build_fixture "$b8" || bad "B8 fixture build failed"
python3 - "$b8/tests/run_all.sh" <<'PYEOF'
import sys
p = sys.argv[1]
lines = open(p).read().splitlines(True)
out, dropped = [], False
for line in lines:
    if not dropped and line.strip().startswith('run_test "test_data_aas"'):
        dropped = True
        continue
    out.append(line)
if not dropped:
    sys.exit("MUTATION DID NOT APPLY")
open(p, "w").writelines(out)
PYEOF
if [[ $? -ne 0 ]]; then
    bad "B8: could not stage the tier-size drift mutant"
else
    rc=$(run_fixture "$b8" "$FIX_ROOT/b8.log")
    assert_case "B8 RED: tier declares 26 but 25 ran -> run FAILS" \
        "$FIX_ROOT/b8.log" "$rc" nonzero \
        "declares 26 test(s) but 25 actually ran" "refusing to report success"
fi

# B9 -- an opt-IN whose dependencies are absent. PGWT_RUN_SNAPSHOTS=1 with no
# tests/web_snapshots/*.png baselines in the fixture: the operator asked for
# the tier and got nothing. (The opt-OUT below stays a plain skip -- B1 already
# exercised that path and went green.)
b9="$FIX_ROOT/b9"; build_fixture "$b9" || bad "B9 fixture build failed"
rc=$(run_fixture "$b9" "$FIX_ROOT/b9.log" PGWT_RUN_SNAPSHOTS=1)
assert_case "B9 RED: PGWT_RUN_SNAPSHOTS=1 with no baselines -> run FAILS" \
    "$FIX_ROOT/b9.log" "$rc" nonzero \
    "test_web_ui_snapshots" "REQUIRED TIER DID NOT RUN" "refusing to report success"

# B10 -- the Playwright twin: opted in, imports forced to fail.
b10="$FIX_ROOT/b10"; build_fixture "$b10" || bad "B10 fixture build failed"
rc=$(run_fixture "$b10" "$FIX_ROOT/b10.log" PGWT_RUN_WEB_UI=1 PYTHONPATH="$b10/pyshim")
assert_case "B10 RED: PGWT_RUN_WEB_UI=1 with playwright/websockets unimportable -> run FAILS" \
    "$FIX_ROOT/b10.log" "$rc" nonzero \
    "Playwright/mock_server tier" "3 test(s) never executed"

# B11 -- the deliberate opt-OUT must still be a skip, not a failure: #528 is
# about accidental absence. Default env (B1) already opts out of both web tiers
# and exited 0; assert the skip lines are there and were counted as skips.
if contains "$FIX_ROOT/b1.log" "test_web_ui — SKIPPED (opt-in: set PGWT_RUN_WEB_UI=1" \
   && contains "$FIX_ROOT/b1.log" "test_web_ui_snapshots — SKIPPED (opt-in: set PGWT_RUN_SNAPSHOTS=1"; then
    ok "B11: deliberate opt-outs remain skips (the opt-in/opt-out distinction is preserved)"
else
    bad "B11: expected the opt-out skip lines in the green control (log: $FIX_ROOT/b1.log)"
fi

# B12 -- --require-live is untouched: a live-section skip is still a failure.
b12="$FIX_ROOT/b12"; build_fixture "$b12" || bad "B12 fixture build failed"
# run_fixture() cannot pass script argv, so invoke directly for this one.
(
    unset PGWT_RUN_WEB_UI PGWT_RUN_SNAPSHOTS
    export PATH="$b12/bin:$PATH"
    bash "$b12/tests/run_all.sh" --require-live > "$FIX_ROOT/b12.log" 2>&1
    echo $? > "$FIX_ROOT/b12.rc"
)
rc=$(cat "$FIX_ROOT/b12.rc")
assert_case "B12: --require-live still fails on a live-section skip" \
    "$FIX_ROOT/b12.log" "$rc" nonzero \
    "--require-live set, but" "Mode:     --require-live"

echo ""
echo "════════════════════════════════════════"
echo "  test_run_all_tier_skip: $pass passed, $fail failed"
echo "════════════════════════════════════════"
[[ "$fail" -eq 0 ]]
