#!/usr/bin/env bash
# test_xval_tolerance_derivation.sh -- unit tests for tests/xval_tolerance.sh
# (issues #294 / #306 / #321, the derivation #309 asked for).
#
# test_cross_validate_tiered.sh needs root, a live postgres, pgbench and a
# built BPF daemon, so it only runs on the Linux gate box. The DERIVATION of
# its tolerance, and the guard that the shipped constant still equals that
# derivation, are pure arithmetic with no live dependency -- so they are
# tested here, on the Mac, under `make check` (same pattern as
# test_cross_validate_rate_gate.sh).
#
# Two halves, and the second is the one that matters:
#
#   TRUE POSITIVE -- the guard goes red when the constant and the derivation
#   disagree. Demonstrated against the exact value master shipped
#   (TOLERANCE=10), so "this gate can fire" is shown rather than asserted.
#
#   FALSE NEGATIVE (the bypass suite) -- every way the guard could be made
#   unable to see, each of which must REFUSE rather than approve:
#     - the script file is missing or unreadable
#     - the TOLERANCE assignment is absent entirely
#     - it is present but commented out (the thing checked is absent, not wrong)
#     - it appears twice (which one gates?)
#     - its value is unparseable ("ten", "8.8pp", "", "-8.8")
#     - a derivation input is empty, non-numeric, or zero
#     - K is outside the tabulated range (extrapolation refused)
#     - awk is missing (127) or present-but-not-executable (126)
#     - the sample count is absent or unparseable
#   Every repo-specific defect this suite is modelled on was a false
#   negative: a check satisfied without checking anything, a guard that
#   approved when it could not resolve its base, a gate that stalled instead
#   of failing. A gate that cannot see must refuse, never approve.
#
# Pure bash + awk + python3. No git, no network, no root, no Linux-only
# dependency.
# Usage: bash tests/test_xval_tolerance_derivation.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/xval_tolerance.sh"

TIERED="$SCRIPT_DIR/test_cross_validate_tiered.sh"
TMP=$(mktemp -d /tmp/pgwt_xvaltol_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

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

eq() {  # eq GOT WANT MSG
    if [[ "$1" == "$2" ]]; then report true "$3"; else
        report false "$3 (got '$1', want '$2')"; fi
}

# mk_fixture DEST SED_EXPR WHAT
# Writes a mutated copy of $TIERED to DEST and REPORTS A FAILURE if the
# mutation was a no-op, i.e. if DEST came out byte-identical to the source.
#
# This exists because of a hole found in this very file: the fixtures below
# were originally built with `sed 's/^TOLERANCE=8.8$/.../'`, whose anchor
# silently matches nothing on any tree whose constant is not 8.8. Run against
# such a tree, three bypass cases quietly tested an UNMODIFIED copy and
# "passed" by asserting nothing -- the exact false-negative shape the rest of
# this suite is written against, reproduced inside the suite itself. An
# unapplied mutation is now a loud failure, so a fixture can never be vacuous.
mk_fixture() {
    local dest="$1" expr="$2" what="$3"
    sed "$expr" "$TIERED" > "$dest"
    if cmp -s "$TIERED" "$dest"; then
        report false "fixture '$what' was NOT built (the mutation matched nothing) — any assertion using it would be vacuous"
        return 1
    fi
    report true "fixture built: $what"
}

# The tolerance line as this tree actually ships it, so the mutations below
# are anchored on what is there rather than on a hard-coded value. Refuses
# loudly if there is no single such line, which would make every fixture
# below meaningless.
TOL_LINE=$(grep -E '^[[:space:]]*TOLERANCE=' "$TIERED" | head -1)
if [[ -z "$TOL_LINE" ]]; then
    echo "FATAL: $TIERED has no TOLERANCE= line; every fixture below would be vacuous." >&2
    exit 1
fi
TOL_RE="^$(printf '%s' "$TOL_LINE" | sed 's/[][\.*^$\/]/\\&/g')\$"

# refuses CMD... -- true when the command exits non-zero AND prints nothing
# on stdout. Both halves are required: a guard that exits non-zero but still
# echoes a number can be consumed as if it had succeeded, which is how
# "refuse" quietly becomes "approve with a default".
refuses() {
    local msg="$1"; shift
    local out rc
    out=$("$@" 2>/dev/null); rc=$?
    if [[ $rc -ne 0 && -z "$out" ]]; then
        report true "$msg"
    else
        report false "$msg (rc=$rc, stdout='$out' — a refusal must be non-zero AND print no value)"
    fi
}

echo "=== test_xval_tolerance_derivation ==="

# ───────────────────────── 1. the derivation itself ─────────────────────────
echo ""
echo "--- the derivation ---"

eq "$(derive_xval_tolerance 6.52 3130 5)" "8.8" \
   "pre-registered inputs (BIAS=6.52, N=3130, K=5) derive 8.8 pp"

eq "$(derive_xval_tolerance "$XVAL_PREREG_BIAS_PP" "$XVAL_PREREG_SAMPLES" "$XVAL_PREREG_K")" "8.8" \
   "the pre-registered constants in xval_tolerance.sh still derive 8.8 pp"

# The bound must not be the achieved number. The five prior captures at this
# operating point max out at 6.3 pp; if someone later "derives" a bound that
# sits on or under the worst observed run, the derivation has been fitted to
# the data and the next ordinary run fails.
awk -v d="$(derive_xval_tolerance 6.52 3130 5)" \
    'BEGIN { exit !(d > 6.3 + 1.0) }' \
  && report true "bound sits >1 pp clear of the worst prior capture (6.3 pp) — not fitted to the achieved number" \
  || report false "bound is within 1 pp of the worst prior capture — it has been fitted to the data"

# Monotonicity: the bound must respond to each input in the right direction.
# A derivation that ignores an input (e.g. drops N through a typo'd awk
# variable) still returns a plausible number, and these three catch that.
awk -v a="$(derive_xval_tolerance 6.52 3130 5)" -v b="$(derive_xval_tolerance 7.52 3130 5)" \
    'BEGIN { exit !(b > a) }' \
  && report true "bound increases with BIAS" || report false "bound ignores BIAS"
awk -v a="$(derive_xval_tolerance 6.52 3130 5)" -v b="$(derive_xval_tolerance 6.52 31300 5)" \
    'BEGIN { exit !(b < a) }' \
  && report true "bound decreases as N grows (more samples, less noise)" \
  || report false "bound ignores N"
awk -v a="$(derive_xval_tolerance 6.52 3130 2)" -v b="$(derive_xval_tolerance 6.52 3130 9)" \
    'BEGIN { exit !(b > a) }' \
  && report true "bound increases with K (more eligible events, wider max)" \
  || report false "bound ignores K"

# The quantile table is the one piece that cannot be recomputed in bash, so
# it is the one piece most likely to rot. Re-derive every entry.
if command -v python3 >/dev/null 2>&1; then
    bad=0
    for k in 1 2 3 4 5 6 7 8 9 10; do
        want=$(python3 -c "
from statistics import NormalDist
q = 0.95 ** (1.0/$k)
print('%.4f' % NormalDist().inv_cdf((1+q)/2))
")
        got=$(xval_z_family_wise "$k")
        [[ "$got" == "$want" ]] || { echo "    K=$k: table says $got, python says $want"; bad=1; }
    done
    [[ $bad -eq 0 ]] \
      && report true "all 10 family-wise quantiles match python3's NormalDist to 4 dp" \
      || report false "the family-wise quantile table has drifted from its formula"
else
    # NOT a skip. python3 is a hard dependency of `make check`; if it is
    # gone, the table is unverifiable and that is a failure, because an
    # unverifiable table is exactly the silent-rot case this test exists for.
    report false "python3 absent — the quantile table cannot be verified, which is a failure, not a skip"
fi

# ───────────────── 2. TRUE POSITIVE: the guard can go red ───────────────────
echo ""
echo "--- true positive: the guard fires on a drifted constant ---"

eq "$(xval_tolerance_matches_script "$TIERED")" "8.8" \
   "the SHIPPED test_cross_validate_tiered.sh matches the derivation"

# Exactly the value master shipped, undriven, for the whole life of the tool.
MASTER_VALUE="$TMP/tiered_master_value.sh"
mk_fixture "$MASTER_VALUE" "s/$TOL_RE/TOLERANCE=10/" "a copy shipping master's undriven TOLERANCE=10"
grep -q '^TOLERANCE=10$' "$MASTER_VALUE" \
  && report true "that fixture really does assign 10" \
  || report false "the fixture does not assign 10 — the red demonstration below would be vacuous"
refuses "TOLERANCE=10 (master's undriven value) is REJECTED by the derivation guard" \
    xval_tolerance_matches_script "$MASTER_VALUE"

# And a near-miss, so the guard is shown to be checking the value rather than
# merely the presence of a digit.
NEAR="$TMP/tiered_near.sh"
mk_fixture "$NEAR" "s/$TOL_RE/TOLERANCE=8.9/" "a copy 0.1 pp off the derived bound"
refuses "TOLERANCE=8.9 (0.1 pp off) is rejected" xval_tolerance_matches_script "$NEAR"

# ─────────── 3. BYPASS SUITE: every way the guard could fail to see ─────────
echo ""
echo "--- bypass suite: a guard that cannot see must refuse ---"

refuses "missing script path refuses"            xval_script_tolerance
refuses "empty script path refuses"              xval_script_tolerance ""
refuses "nonexistent script refuses"             xval_script_tolerance "$TMP/does_not_exist.sh"

UNREADABLE="$TMP/unreadable.sh"
printf 'TOLERANCE=8.8\n' > "$UNREADABLE"; chmod 000 "$UNREADABLE"
if [[ -r "$UNREADABLE" ]]; then
    # Running as root, or a filesystem that ignores the mode bit: the case
    # cannot be constructed here, and saying so is better than a green tick
    # for an assertion that was never made.
    report false "could not construct an unreadable file (running as root?) — this bypass case was NOT exercised"
else
    refuses "unreadable script refuses"          xval_script_tolerance "$UNREADABLE"
fi
chmod 644 "$UNREADABLE"

# The thing being checked is ABSENT rather than wrong.
NO_ASSIGN="$TMP/no_assignment.sh"
grep -v '^TOLERANCE=' "$TIERED" > "$NO_ASSIGN"
! grep -qE '^[[:space:]]*TOLERANCE=' "$NO_ASSIGN" \
  && report true "fixture built: TOLERANCE assignment deleted outright" \
  || report false "fixture NOT built — the absent-value case below is vacuous"
refuses "a script with NO TOLERANCE assignment refuses (absent is not a pass)" \
    xval_script_tolerance "$NO_ASSIGN"

COMMENTED="$TMP/commented.sh"
mk_fixture "$COMMENTED" "s/$TOL_RE/#&/" "the TOLERANCE line commented out"
refuses "a COMMENTED-OUT TOLERANCE refuses (disabled is not satisfied)" \
    xval_script_tolerance "$COMMENTED"

INDENT_COMMENT="$TMP/indent_commented.sh"
mk_fixture "$INDENT_COMMENT" "s/$TOL_RE/   # &/" "the TOLERANCE line indented and commented out"
refuses "an indented commented-out TOLERANCE refuses" \
    xval_script_tolerance "$INDENT_COMMENT"

DOUBLE="$TMP/double.sh"
mk_fixture "$DOUBLE" "s/$TOL_RE/&\nTOLERANCE=99/" "a second, conflicting TOLERANCE assignment"
eq "$(grep -cE '^[[:space:]]*TOLERANCE=' "$DOUBLE")" "2" \
   "that fixture really does carry two assignments"
refuses "two TOLERANCE assignments refuse (ambiguous which one gates)" \
    xval_script_tolerance "$DOUBLE"

for bad in ten 8.8pp "" -8.8 '$TOL' 1e1; do
    BADV="$TMP/badv.sh"
    { grep -v '^TOLERANCE=' "$TIERED"; printf 'TOLERANCE=%s\n' "$bad"; } > "$BADV"
    refuses "unparseable TOLERANCE='$bad' refuses" xval_script_tolerance "$BADV"
done

# An indented but genuinely live assignment must still be FOUND, so the
# comment-rejection above is not just "anything with leading space is
# ignored" -- that would be a bypass of its own.
INDENT_LIVE="$TMP/indent_live.sh"
{ grep -v '^TOLERANCE=' "$TIERED"; printf '    TOLERANCE=8.8\n'; } > "$INDENT_LIVE"
eq "$(xval_script_tolerance "$INDENT_LIVE")" "8.8" \
   "an indented LIVE assignment is still found (comment rejection is not whitespace blindness)"

echo ""
echo "--- bypass suite: unusable derivation inputs ---"
refuses "no args refuses"                    derive_xval_tolerance
refuses "two args refuses"                   derive_xval_tolerance 6.52 3130
refuses "four args refuses"                  derive_xval_tolerance 6.52 3130 5 7
refuses "empty BIAS refuses"                 derive_xval_tolerance "" 3130 5
refuses "non-numeric BIAS refuses"           derive_xval_tolerance abc 3130 5
refuses "negative BIAS refuses"              derive_xval_tolerance -1 3130 5
refuses "empty N refuses"                    derive_xval_tolerance 6.52 "" 5
refuses "N=0 refuses (no division by zero)"  derive_xval_tolerance 6.52 0 5
refuses "non-numeric N refuses"              derive_xval_tolerance 6.52 lots 5
refuses "K=0 refuses (nothing was compared)" derive_xval_tolerance 6.52 3130 0
refuses "K=11 refuses (extrapolation off the table)" derive_xval_tolerance 6.52 3130 11
refuses "non-numeric K refuses"              derive_xval_tolerance 6.52 3130 five
refuses "K table refuses K=0"                xval_z_family_wise 0
refuses "K table refuses K=11"               xval_z_family_wise 11
refuses "K table refuses an empty K"         xval_z_family_wise ""

echo ""
echo "--- bypass suite: a dependency exiting 126 / 127 ---"
# The dangerous shape is not "awk is wrong", it is "awk is gone and the
# derivation returns an empty string that a caller compares against another
# empty string and calls agreement". Both exit codes get their own stub.
for mode in 127 126; do
    STUBDIR="$TMP/stub$mode"
    mkdir -p "$STUBDIR"
    if [[ "$mode" == 127 ]]; then
        # Nothing named awk on PATH at all.
        :
    else
        # Present but not executable -> 126.
        printf '#!/bin/sh\nexit 0\n' > "$STUBDIR/awk"
        chmod 000 "$STUBDIR/awk"
    fi
    out=$(PATH="$STUBDIR" bash -c "
        source '$SCRIPT_DIR/xval_tolerance.sh'
        derive_xval_tolerance 6.52 3130 5
    " 2>/dev/null)
    rc=$?
    if [[ $rc -ne 0 && -z "$out" ]]; then
        report true "awk unavailable (exit $mode): derivation refuses instead of returning an empty bound"
    else
        report false "awk unavailable (exit $mode): rc=$rc out='$out' — a missing dependency must refuse"
    fi
    # And the composed guard must refuse too, not silently compare "" to "".
    out=$(PATH="$STUBDIR" bash -c "
        source '$SCRIPT_DIR/xval_tolerance.sh'
        xval_tolerance_matches_script '$TIERED'
    " 2>/dev/null)
    rc=$?
    if [[ $rc -ne 0 && -z "$out" ]]; then
        report true "awk unavailable (exit $mode): the match guard refuses rather than comparing empty to empty"
    else
        report false "awk unavailable (exit $mode): match guard rc=$rc out='$out'"
    fi
    chmod 755 "$STUBDIR/awk" 2>/dev/null || true
done

echo ""
echo "--- the capture-thickness floor (round 2: re-specified a priori) ---"
eq "$(derive_xval_sample_floor)" "2587" \
   "the pre-registered criterion (10% SE inflation off N=3130) derives a floor of 2587"
eq "$(derive_xval_sample_floor 1.10 3130)" "2587" \
   "the floor is base/inflation^2, rounded up"

# Constraint (b): the floor must sit >= 4 sd below the mean of the gate
# harness's capture-thickness distribution. This is the constraint that
# round 1 violated -- its floor sat ABOVE the observed minimum.
awk -v m="$(xval_floor_sd_margin)" -v k="$XVAL_FLOOR_MIN_SD_BELOW" \
    'BEGIN { exit !(m >= k) }' \
  && report true "the floor is $(xval_floor_sd_margin) sd below the harness mean, clearing the required ${XVAL_FLOOR_MIN_SD_BELOW} sd" \
  || report false "the floor is only $(xval_floor_sd_margin) sd below the harness mean — ordinary variation would trip it"

# The selection rule has to actually bind, or "I chose 10%" is decoration.
# 5% must FAIL constraint (b); 10% must pass. If both passed, the rule would
# not pin a unique answer and the choice would be arbitrary.
awk -v m="$(xval_floor_sd_margin "$(derive_xval_sample_floor 1.05)")" -v k="$XVAL_FLOOR_MIN_SD_BELOW" \
    'BEGIN { exit !(m < k) }' \
  && report true "the 5% tolerance is genuinely excluded by the 4-sd constraint ($(xval_floor_sd_margin "$(derive_xval_sample_floor 1.05)") sd) — the selection rule binds" \
  || report false "the 5% tolerance also clears 4 sd, so the selection rule does not pin a unique answer"

# The floor must be BELOW the tolerance's own N. A floor at or above it is
# exactly the round-1 mis-specification, and it must be impossible to
# reintroduce by editing the inflation constant alone.
awk -v f="$(derive_xval_sample_floor)" -v n="$XVAL_PREREG_SAMPLES" 'BEGIN { exit !(f < n) }' \
  && report true "the floor (2587) is below the tolerance's N (3130) — the two roles are separate numbers" \
  || report false "the floor is at or above the tolerance's N — the round-1 conflation is back"
refuses "an SE-inflation tolerance below 1.0 refuses (it would put the floor above the derivation's N)" \
    derive_xval_sample_floor 0.9 3130

# Every capture measured on the gate's own harness at the shipped operating
# point must clear the floor. These are the 11 observations the floor was
# specified AGAINST (not fitted to) -- round 1 refused 5 of them.
bad=0
for n in 3033 3274 2922 3109 3059 3165 3069 3133 3211 3233 2918; do
    refuse_unless_samples_sufficient "$n" >/dev/null 2>&1 || { echo "    N=$n refused"; bad=1; }
done
[[ $bad -eq 0 ]] \
  && report true "all 11 measured gate-harness captures (N 2918-3274) clear the floor" \
  || report false "the floor still refuses an ordinary gate-harness capture"

# ...and the floor must still REFUSE a genuinely broken capture, or it has
# been relaxed into uselessness. 356 is the N of a real --esc-seconds 8 run.
refuses "a 356-sample capture (real --esc-seconds 8 run) is still refused" \
    refuse_unless_samples_sufficient 356
refuses "a 2586-sample capture (one below the floor) is refused" \
    refuse_unless_samples_sufficient 2586
eq "$(refuse_unless_samples_sufficient 2587)" "2587" \
   "a capture exactly at the floor is accepted"

# The tolerance must NOT have moved while the floor was re-specified. The two
# are independent problems and mixing them would hide both.
eq "$(derive_xval_tolerance "$XVAL_PREREG_BIAS_PP" "$XVAL_PREREG_SAMPLES" "$XVAL_PREREG_K")" "8.8" \
   "re-specifying the floor did NOT move the tolerance (still 8.8 pp)"
eq "$(xval_script_tolerance "$TIERED")" "8.8" \
   "the live script still ships 8.8 pp"

echo ""
echo "--- bypass suite: sample-count blindness ---"
eq "$(extract_samples_in_window 'Samples in window: 3456')" "3456" \
   "sample count is parsed from cross_validate output"
eq "$(extract_samples_in_window '')" "?" \
   "EMPTY comparator output yields '?', never 0 (empty input must not read as a thin-but-known capture)"
eq "$(extract_samples_in_window 'Top-5 event overlap: 5/5')" "?" \
   "output without the samples line yields '?'"
eq "$(extract_samples_in_window 'Samples in window: lots')" "?" \
   "a malformed sample count yields '?'"
refuses "an unparseable sample count '?' refuses (unknown N is not sufficient N)" \
    refuse_unless_samples_sufficient "?"
refuses "an EMPTY sample count refuses" refuse_unless_samples_sufficient ""
refuses "a sample count below the derivation's N refuses" \
    refuse_unless_samples_sufficient 3129 3130
eq "$(refuse_unless_samples_sufficient 3130 3130)" "3130" \
   "a sample count exactly at the derivation's N is accepted"
eq "$(refuse_unless_samples_sufficient 79679 3130)" "79679" \
   "a 200 Hz-sized capture is accepted"
refuses "an unparseable MINIMUM refuses too (a blind floor is not a floor)" \
    refuse_unless_samples_sufficient 5000 "?"
refuses "a non-numeric SE inflation refuses"   derive_xval_sample_floor abc 3130
refuses "an empty SE inflation refuses"        derive_xval_sample_floor "" 3130
refuses "a zero base sample count refuses"     derive_xval_sample_floor 1.10 0
refuses "a non-numeric base refuses"           derive_xval_sample_floor 1.10 lots
refuses "a non-numeric floor refuses in the sd-margin check" xval_floor_sd_margin abc

# A zero harness sd would make EVERY floor look infinitely safe -- a check
# that cannot fail. It must refuse rather than report a huge margin.
out=$(bash -c "
    source '$SCRIPT_DIR/xval_tolerance.sh'
    XVAL_HARNESS_N_SD=0
    xval_floor_sd_margin 2587
" 2>/dev/null); rc=$?
if [[ $rc -ne 0 && -z "$out" ]]; then
    report true "a zero harness sd refuses (every floor would otherwise look infinitely safe)"
else
    report false "a zero harness sd gave rc=$rc out='$out' — the 4-sd constraint would be unfalsifiable"
fi

# And awk going missing must take the floor down too, not yield an empty
# "floor" that compares equal to an empty observed count.
for mode in 127 126; do
    STUBDIR="$TMP/fstub$mode"; mkdir -p "$STUBDIR"
    if [[ "$mode" == 126 ]]; then printf '#!/bin/sh\nexit 0\n' > "$STUBDIR/awk"; chmod 000 "$STUBDIR/awk"; fi
    out=$(PATH="$STUBDIR" bash -c "
        source '$SCRIPT_DIR/xval_tolerance.sh'
        refuse_unless_samples_sufficient 3000
    " 2>/dev/null); rc=$?
    if [[ $rc -ne 0 && -z "$out" ]]; then
        report true "awk unavailable (exit $mode): the thickness check refuses rather than passing an underivable floor"
    else
        report false "awk unavailable (exit $mode): rc=$rc out='$out'"
    fi
    chmod 755 "$STUBDIR/awk" 2>/dev/null || true
done

# ───────────── 4. the live script actually consults the guard ───────────────
# The guard existing and the gate USING it are different claims. #309's
# original defect was a decision function that was correct in isolation and
# wired to nothing, so this checks the wiring by name rather than trusting it.
echo ""
echo "--- wiring: test_cross_validate_tiered.sh consults the guard ---"
grep -q 'source "\$SCRIPT_DIR/xval_tolerance.sh"' "$TIERED" \
  && report true "the live script sources xval_tolerance.sh" \
  || report false "the live script does NOT source xval_tolerance.sh — the guard is dead code"
grep -q 'xval_tolerance_matches_script' "$TIERED" \
  && report true "the live script calls xval_tolerance_matches_script" \
  || report false "the live script never calls the match guard"
grep -q 'refuse_unless_samples_sufficient' "$TIERED" \
  && report true "the live script calls refuse_unless_samples_sufficient" \
  || report false "the live script never checks capture thickness"
grep -q 'derive_xval_sample_floor' "$TIERED" \
  && report true "the live script derives its thickness floor instead of hard-coding one" \
  || report false "the live script does not derive the floor — a literal would drift from its criterion"
grep -q 'SAMP_MIN="\$XVAL_PREREG_SAMPLES"' "$TIERED" \
  && report false "the live script still uses the tolerance's N as the thickness floor (the round-1 conflation)" \
  || report true "the live script no longer uses the tolerance's N as the thickness floor"
grep -q 'exit 1' <<<"$(grep -A6 'ERROR: this script.s TOLERANCE does not match' "$TIERED")" \
  && report true "a drifted constant makes the live script EXIT, not warn" \
  || report false "the live script warns but continues on a drifted constant"

# The guard resolves its own path absolutely. A bare ${BASH_SOURCE[0]} works
# only because run_all.sh passes an absolute path; a caller using a relative
# path from another directory would leave the guard unable to READ the file
# it is checking, which refuses -- a spurious gate failure, not a silent
# pass, but a failure nobody could diagnose.
grep -q 'xval_tolerance_matches_script "\$SCRIPT_DIR/' "$TIERED" \
  && report true "the guard resolves the script path via \$SCRIPT_DIR, not a bare \$BASH_SOURCE" \
  || report false "the guard uses an unanchored path — a relative invocation from another cwd would refuse spuriously"

# Prove it, rather than only grepping for the idiom: the guard must return
# the same answer from an unrelated working directory.
( cd / && source "$SCRIPT_DIR/xval_tolerance.sh" \
    && [[ "$(xval_tolerance_matches_script "$TIERED")" == "8.8" ]] ) \
  && report true "the guard returns 8.8 when invoked from / (cwd-independent)" \
  || report false "the guard is cwd-dependent"

echo ""
echo "===== summary ====="
echo "  run=$tests_run passed=$tests_passed failed=$tests_failed"
if [[ $tests_failed -eq 0 && $tests_run -gt 0 ]]; then
    echo "PASS: test_xval_tolerance_derivation ($tests_passed assertions)"
    exit 0
fi
echo "FAIL: test_xval_tolerance_derivation ($tests_failed of $tests_run failed)"
exit 1
