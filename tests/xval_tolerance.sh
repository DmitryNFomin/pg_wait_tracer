#!/usr/bin/env bash
# xval_tolerance.sh -- the DERIVATION behind test_cross_validate_tiered.sh's
# tolerance, as executable arithmetic plus a guard that the shipped constant
# still equals it (issues #294 / #306 / #321; the derivation #309 asked for
# and never got).
#
# Why this file exists
# ===================
# test_cross_validate_tiered.sh shipped `TOLERANCE=10` from the A4 tool's
# first commit (eaf31b0) with no derivation anywhere in the repo, and its own
# comment disowned it: "treat 10pp as a value someone should revisit, not a
# measured bound". A gate whose threshold nobody derived is not a gate; it is
# a number that happens not to fire. Worse, a prose derivation rots silently
# the first time someone edits the constant. So the derivation lives here as
# code, the constant is checked against it by `make check` on every run
# (xval_tolerance_matches_script below), and the two cannot drift.
#
# What the gated metric actually is
# =================================
# cross_validate prints
#   "Max share disagreement (events >= 2% exact share): D pp (EVENT)"
# D is the MAXIMUM over the eligible events (exact time-share >= --min-share,
# default 2%) of |exact_share - sampled_share|. Two consequences drive the
# whole derivation and were both missed by earlier attempts to read this
# number:
#
#   (a) D is an ORDER STATISTIC over a changing set of events, not the error
#       of one fixed event. #321 was opened as "a stable 6 pp CPU regression"
#       and then WITHDRAWN by its own author for exactly this reason: across
#       runs the maximising event changes (one gate-1 run maximised on
#       `transactionid` at 0.4 pp), so D has been observed at 0.4, 3.7, 6.0,
#       6.9, 7.2 and 7.6 pp on one box. Comparing two single draws of D is
#       close to meaningless. A bound on D must therefore be a FAMILY-WISE
#       bound over the eligible events, not a per-event one.
#   (b) D is bounded below by the systematic bias of its worst event. The
#       sampled tier reads CPU low (#306, still open), so no amount of
#       sampling buys D down to zero; a bound that ignores the bias term is
#       unreachable by construction.
#
# The derivation
# ==============
# Bound = BIAS + z(K) * SE,   SE = 100 * sqrt(p(1-p) / N)  percentage points
#
#   BIAS  the systematic CPU under-read at the SHIPPED operating point
#         (10 Hz, SCHED_OTHER, 8 pgbench clients). Taken as the largest
#         PUBLISHED, PRE-EXISTING estimate at that operating point:
#         6.52 pp, #306 arm A median (n=3, interleaved A/B/C, one
#         cross_validate binary for all nine runs).
#         NOT 3.9 pp: that is #306 arm B, measured under `chrt -f 10`
#         SCHED_FIFO, which the product does not ship. Using the SCHED_FIFO
#         residual would derive a bound for a configuration nobody runs.
#   N     DB-time samples the comparator counts in the window -- the
#         "Samples in window:" line, i.e. (tick, backend) observations, NOT
#         ticks. 3130 is the SMALLEST of the five observed at the shipped
#         operating point (3130/3445/3533/3541/3682,
#         tests/results/sampler-294/round2/gate1-rep10-fix/r*/xval.txt), so
#         the SE is the largest those captures support.
#   p     share at which SE is maximal, p = 0.5. The maximising event's exact
#         share is ~0.40-0.47 in every observed capture, so p=0.5 is a small
#         deliberate over-estimate rather than a fitted value.
#   z(K)  two-sided quantile giving 95% FAMILY-WISE coverage over K eligible
#         events: (2*Phi(z) - 1)^K = 0.95. Table below. K = 5 is measured,
#         not assumed: CPU, WALWrite, transactionid, WalSync and
#         CheckpointWriteDelay clear the 2% floor in 5 of 5 captures.
#
# Arithmetic at the pre-registered inputs:
#   SE   = 100 * sqrt(0.25 / 3130)      = 0.8937 pp
#   z(5) = 2.5688                        (0.95^(1/5) = 0.989794 -> Phi = 0.994897)
#   bound = 6.52 + 2.5688 * 0.8937       = 6.52 + 2.2958 = 8.8158 -> 8.8 pp
#
# Pre-registered 2026-10-25, BEFORE the confirmation runs: this file and the
# TOLERANCE change landed in their own commit, with no measurement of this
# branch's own taken yet. A bound chosen after seeing the result is void, and
# git order is the only durable proof of which came first. In particular the
# bound is NOT the achieved number: the five prior captures max out at 6.3 pp,
# and 8.8 sits 2.5 pp above that by derivation, not by rounding up to clear a
# run.
#
# How this differs from the 8 pp sketch the task started from
# ===========================================================
# That sketch read: 600 samples -> SE ~2 pp, 2*SE ~4 pp, plus a 3.9 pp
# residual, ~8 pp. Same total, different and better-supported components:
#   - N is 3130, not 600. Each tick samples every target backend, so a 60 s
#     10 Hz window yields thousands of samples, not 600. CHECKED against the
#     prior data rather than assumed: the measured run-to-run sd of D at this
#     operating point is 0.696 pp (n=5), which is consistent with the
#     0.89 pp per-sample binomial SE and REFUTES the 2.04 pp a 600-tick model
#     predicts. The sketch's SE was ~2.3x too large.
#   - the multiplier is z(5)=2.57, not 2.0: D is a max over 5 eligible
#     events, so a 2-sigma per-event figure gives ~23% family-wise coverage,
#     not 95%.
#   - BIAS is 6.52 pp (shipped SCHED_OTHER), not 3.9 pp (SCHED_FIFO).
# The two errors offset, which is why both routes land near 8. Agreement
# between a wrong decomposition and a right one is a coincidence, not
# corroboration, and is recorded here so nobody cites the sketch.
#
# A two-regime story you will be tempted to re-derive, and should not
# =====================================================================
# The confirmation runs recorded two groups: one with 10 s between runs whose
# loadavg1 read 5.10-6.37, and one that waited for loadavg1 < 0.60 and read
# 0.46-0.55. Round 1 of this work called those "loaded" and "quiet", treated
# them as two regimes, and concluded the bound held in one and failed in the
# other. THAT CONCLUSION WAS WRONG and is retracted here.
#
# Both figures are PRE-RUN readings. The test drives the box itself:
# pgbench -c 8 on 4 vCPU puts loadavg1 at ~6 during the capture whatever it
# was beforehand (measured: 0.35 -> 4.51 across one run, 4.42 -> 6.02 across
# another), and inside tests/run_all.sh this test runs at loadavg1 median
# 6.60 (n=135 samples over its 271 s window, min 2.51, max 7.69). So the
# "quiet" group capture at ~6 too, and all three groups -- both standalone
# sets and the real run_all invocation -- are the SAME regime. What differed
# between the two standalone sets was only pre-run residue from the previous
# run.
#
# Consequence: there is no measured load-dependence here, and the single
# out-of-bound draw is not evidence of one. Do not add a load-robust variance
# term or a quiet precondition on the strength of those pre-run numbers --
# that would be designing against a cause the measurement refutes.
#
# The honest limit on the exceedance rate
# =======================================
# Across all 17 10 Hz captures taken on this harness, 1 exceeded the bound:
# 5.9% against the 5% family-wise design target, with the floor contributing
# 0 of 17 refusals. That agreement is reassuring but WEAK: 1/17 is a wide
# estimate of a 5% tail, and the single run_all cell inside it is n=1, which
# is exactly the kind of figure #321 was built on. Treat the rate as
# provisional and re-check it as runs accumulate, rather than as settled.
# This is a note for whoever next looks, not a reason to keep measuring.
#
# What this bound does NOT cover -- read before trusting it
# ========================================================
#  - It is derived for the SHIPPED 10 Hz rate only. The script applies it to
#    every rate in its sweep because the published post-#310 gate-1 medians
#    fall with rate (6.0 / 6.4 / 5.5 / 2.5 pp at 10 / 50 / 100 / 200 Hz), so
#    the 10 Hz bound is the loosest of the four; a per-rate bound would need
#    its own n>=5 per rate and is not derived here. Do not read a 200 Hz pass
#    as "within its derived bound".
#  - It is a bound on the metric's SPREAD AND BIAS, not a certificate of
#    accuracy. #306's residual under-read is OPEN and is an INPUT to this
#    number: 6.52 of the 8.8 pp is known bias, disclosed, not explained.
#  - It cannot see a capture whose eligible-event set collapsed. If CPU falls
#    below the 2% floor, or the window yields almost no samples, D is a max
#    over few or no events and can be small while the sampler is badly
#    wrong. That is a cross_validate-side gap, not something a tolerance can
#    close; refuse_unless_samples_sufficient below is the part of it this
#    file CAN check.
#
# Pure bash + awk. No git, no network, no Linux-only dependency, no root:
# sourced by test_cross_validate_tiered.sh on the gate box AND by
# tests/test_xval_tolerance_derivation.sh under `make check` on the Mac.

# ---------------------------------------------------------------------------
# Pre-registered inputs. These are the ONLY place the derivation's inputs
# live; test_cross_validate_tiered.sh's TOLERANCE is checked against what
# they produce, so editing either one alone turns `make check` red.
# ---------------------------------------------------------------------------
XVAL_PREREG_BIAS_PP=6.52     # #306 arm A median, shipped SCHED_OTHER 10 Hz
XVAL_PREREG_SAMPLES=3130     # smallest observed "Samples in window" at that point
XVAL_PREREG_K=5              # eligible events (exact share >= 2%), 5 of 5 captures

# ---------------------------------------------------------------------------
# The capture-thickness floor, re-specified 2026-10-25 (round 2).
#
# WHAT WAS WRONG THE FIRST TIME. This floor was originally set to
# XVAL_PREREG_SAMPLES itself (3130), i.e. to the MINIMUM of a prior n=5. Two
# errors in one number:
#
#   1. Wrong harness. 3130 was the thinnest capture from diag294's own
#      pgbench driver (N 3130-3682). test_cross_validate_tiered.sh's
#      workload runs THINNER: measured on the gate's own harness, n=11,
#      N = 2918-3274, mean 3102, sd 117 (CV 3.8%). Transplanting a figure
#      between two harnesses known to read differently is exactly the error
#      #321 was built on, and it was repeated here.
#   2. Wrong statistic. The minimum of five observations is not a lower
#      bound on the sixth. The floor fired on 5 of 10 ordinary captures --
#      a false-FAILURE mechanism, not a detection.
#
# TWO ROLES, NOW SEPARATED. XVAL_PREREG_SAMPLES plays one role only: the N at
# which the tolerance's sampling error was computed. It does NOT also serve as
# a validity gate, which is what conflating them produced. The floor below is
# derived independently and the tolerance is untouched by it.
#
# THE CRITERION, fixed before confirming (two constraints, which together pin
# a unique answer -- so this is a selection rule, not a number chosen to let
# some observations through):
#
#   (a) The capture's binomial SE must not exceed the SE the bound budgets by
#       more than a round tolerance. SE_budget = 100*sqrt(0.25/3130) =
#       0.8937 pp.
#   (b) The floor must sit at least 4 sd below the mean of the gate harness's
#       own capture-thickness distribution. Its job is to catch a
#       STRUCTURALLY BROKEN capture -- stalled daemon, mis-aligned window,
#       dead workload -- not ordinary run-to-run variation in how many
#       backends are in DB Time per tick. At 4 sd, ordinary variation trips
#       it less than once in 10,000 runs.
#
#   tolerance | SE_max   | floor | sd below mean | verdict
#      5%     | 0.9384   | 2839  |  2.25         | fails (b)
#     10%     | 0.9831   | 2587  |  4.39         | PASSES both -> chosen
#     20%     | 1.0725   | 2174  |  7.92         | passes, but see cost below
#
# Take the TIGHTEST round tolerance that clears (b): 10%, floor = 2587.
#   N_floor = 0.25 * 100^2 / (1.10 * 0.8937)^2 = 2500 / 0.96648 = 2586.8 -> 2587
#
# THE COST, STATED RATHER THAN HIDDEN. Admitting a capture at exactly N=2587
# admits one whose own derived bound would be 6.52 + 2.5688*0.9831 = 9.05 pp,
# while the gate still holds it to 8.8 pp. The gate is therefore up to 0.25 pp
# TIGHTER than the derivation permits at the floor -- conservative, which is
# the safe direction, and the reason it is acceptable to leave the tolerance
# alone. The 20% row would cost 0.47 pp of the same optimism, which is why the
# tightest qualifying tolerance is taken rather than the most comfortable one.
#
# Applied at EVERY rate, not just 10 Hz. At 50/100/200 Hz the observed N is
# 15,718 / 30,527 / 62,864, so the floor is cleared by an order of magnitude
# and cannot cause a false failure there -- but it still catches a
# structurally broken capture at those rates, which the previous
# "any N >= 1" rule did not.
XVAL_FLOOR_SE_INFLATION=1.10  # (a): round tolerance on the model input
XVAL_HARNESS_N_MEAN=3102      # (b): gate harness, n=11, shipped operating point
XVAL_HARNESS_N_SD=117         #      sd of the same 11 captures
XVAL_FLOOR_MIN_SD_BELOW=4     # (b): required margin, in sd, below that mean

# derive_xval_sample_floor [SE_INFLATION] [PREREG_SAMPLES]
# Echoes the floor (integer samples) and returns 0; refuses (exit 2) on any
# unusable input or unusable awk, same discipline as derive_xval_tolerance.
# Rounds UP (ceil): rounding a floor down would admit captures the criterion
# excludes.
derive_xval_sample_floor() {
    local infl="${1-$XVAL_FLOOR_SE_INFLATION}" base="${2-$XVAL_PREREG_SAMPLES}" out
    _xval_is_number "$infl" || { echo "REFUSE: SE inflation tolerance is not a number: '$infl'" >&2; return 2; }
    _xval_is_number "$base" || { echo "REFUSE: base sample count is not a number: '$base'" >&2; return 2; }
    # An inflation tolerance below 1.0 would put the floor ABOVE the N the
    # bound was derived at, which is the original bug in a new costume.
    awk -v i="$infl" 'BEGIN { exit !(i >= 1.0) }' \
      || { echo "REFUSE: SE inflation tolerance must be >= 1.0 (a floor above the derivation's own N is the mis-specification this replaces), got '$infl'" >&2; return 2; }
    [[ "${base%%.*}" -ge 1 ]] || { echo "REFUSE: base sample count must be >= 1, got '$base'" >&2; return 2; }
    # floor = 0.25*100^2 / (infl * 100*sqrt(0.25/base))^2  ==  base / infl^2
    out=$(awk -v b="$base" -v i="$infl" \
        'BEGIN { v = b / (i*i); printf "%d", (v == int(v) ? v : int(v) + 1) }' 2>/dev/null)
    local rc=$?
    [[ $rc -eq 0 ]] || { echo "REFUSE: awk failed (exit $rc) computing the sample floor" >&2; return 2; }
    _xval_is_number "$out" || { echo "REFUSE: awk produced no usable floor (got '$out')" >&2; return 2; }
    [[ "$out" -ge 1 ]] || { echo "REFUSE: computed floor is not positive ('$out')" >&2; return 2; }
    echo "$out"
}

# xval_floor_sd_margin [FLOOR]
# Echoes how many sd below the harness mean the floor sits, to 2 dp -- the
# (b) constraint, as a number a reviewer can check rather than a claim in a
# comment. Refuses if the harness sd is zero or unusable (a zero sd would
# make every floor "infinitely safe", which is the shape of a check that
# cannot fail).
xval_floor_sd_margin() {
    local floor="${1-$(derive_xval_sample_floor)}" out
    _xval_is_number "$floor" || { echo "REFUSE: floor is not a number: '$floor'" >&2; return 2; }
    _xval_is_number "$XVAL_HARNESS_N_MEAN" || { echo "REFUSE: harness N mean unusable" >&2; return 2; }
    _xval_is_number "$XVAL_HARNESS_N_SD" || { echo "REFUSE: harness N sd unusable" >&2; return 2; }
    [[ "${XVAL_HARNESS_N_SD%%.*}" -ge 1 ]] || { echo "REFUSE: harness N sd is zero; every floor would look infinitely safe" >&2; return 2; }
    out=$(awk -v m="$XVAL_HARNESS_N_MEAN" -v s="$XVAL_HARNESS_N_SD" -v f="$floor" \
        'BEGIN { printf "%.2f", (m - f) / s }' 2>/dev/null) || {
        echo "REFUSE: awk failed computing the sd margin" >&2; return 2; }
    _xval_is_number "$out" || { echo "REFUSE: sd margin is not a usable number ('$out')" >&2; return 2; }
    echo "$out"
}

# xval_z_family_wise K
# Two-sided normal quantile z with (2*Phi(z)-1)^K = 0.95, i.e. 95% coverage
# of the MAXIMUM of K deltas. Tabulated because neither bash nor POSIX awk
# has an inverse normal CDF; tests/test_xval_tolerance_derivation.sh
# re-derives every entry with python3's NormalDist and fails on any
# mismatch, so the table cannot silently rot.
#
# REFUSES (exit 1, no output) outside K=1..10 rather than extrapolating: a
# capture with more than 10 eligible events is outside anything measured
# here, and a bound invented for it would be a guess wearing a derivation's
# clothes.
xval_z_family_wise() {
    local k="${1-}"
    case "$k" in
        1)  echo 1.9600 ;;
        2)  echo 2.2365 ;;
        3)  echo 2.3877 ;;
        4)  echo 2.4909 ;;
        5)  echo 2.5688 ;;
        6)  echo 2.6310 ;;
        7)  echo 2.6828 ;;
        8)  echo 2.7270 ;;
        9)  echo 2.7655 ;;
        10) echo 2.7996 ;;
        *)  return 1 ;;
    esac
}

# _xval_is_number STRING -- true for a non-negative decimal number only.
# Deliberately strict: no sign, no exponent, no whitespace, no "8.8pp", no
# empty string. Every value this file compares or multiplies goes through
# here first, because the dangerous failure is not a wrong number, it is an
# EMPTY one on both sides of a comparison reading as agreement.
_xval_is_number() {
    local s="${1-}"
    [[ -n "$s" ]] || return 1
    [[ "$s" =~ ^[0-9]+(\.[0-9]+)?$ ]]
}

# derive_xval_tolerance BIAS_PP SAMPLES K
# Echoes the bound in pp to 1 decimal place and returns 0.
# On ANY unusable input or unusable awk, prints "REFUSE: <reason>" on stderr
# and returns 2. It never falls back to a default, and never echoes a number
# it did not compute -- a derivation that cannot compute must refuse, because
# the alternative (echoing 0, or echoing nothing and letting the caller
# compare empty to empty) reads as approval.
derive_xval_tolerance() {
    if [[ $# -ne 3 ]]; then
        echo "REFUSE: derive_xval_tolerance needs exactly 3 args (BIAS_PP SAMPLES K), got $#" >&2
        return 2
    fi
    local bias="$1" samples="$2" k="$3" z out

    _xval_is_number "$bias" || { echo "REFUSE: BIAS_PP is not a non-negative number: '$bias'" >&2; return 2; }
    _xval_is_number "$samples" || { echo "REFUSE: SAMPLES is not a non-negative number: '$samples'" >&2; return 2; }
    _xval_is_number "$k" || { echo "REFUSE: K is not a non-negative number: '$k'" >&2; return 2; }
    # SAMPLES=0 would divide by zero; awk would yield "inf" or "nan", and
    # "nan <= tolerance" is false in C but the string is a parse hazard here.
    # Refuse up front instead.
    [[ "${samples%%.*}" -ge 1 ]] || { echo "REFUSE: SAMPLES must be >= 1, got '$samples'" >&2; return 2; }
    [[ "${k%%.*}" -ge 1 ]] || { echo "REFUSE: K must be >= 1 (no eligible events = nothing was compared), got '$k'" >&2; return 2; }

    z=$(xval_z_family_wise "${k%%.*}") || {
        echo "REFUSE: no family-wise quantile tabulated for K=$k (table covers 1..10); refusing to extrapolate" >&2
        return 2
    }

    # p = 0.5 maximises sqrt(p(1-p)), so 0.25 is hard-coded as the numerator
    # rather than taking an observed share: see the header.
    out=$(awk -v b="$bias" -v n="$samples" -v z="$z" \
        'BEGIN { printf "%.1f", b + z * 100.0 * sqrt(0.25 / n) }' 2>/dev/null)
    local awk_rc=$?
    # 127 = awk not found, 126 = found but not executable. Both land here,
    # and both must refuse rather than return an empty "bound".
    if [[ $awk_rc -ne 0 ]]; then
        echo "REFUSE: awk failed (exit $awk_rc) while computing the bound" >&2
        return 2
    fi
    _xval_is_number "$out" || {
        echo "REFUSE: awk produced no usable number (got '$out')" >&2
        return 2
    }
    echo "$out"
}

# xval_script_tolerance SCRIPT_PATH
# Echoes the TOLERANCE value assigned in SCRIPT_PATH and returns 0.
# Refuses (exit 2) when the file is unreadable, when there is no uncommented
# `TOLERANCE=<number>` assignment at all, when there is more than one, or
# when the value is not a number.
#
# "No assignment at all" is the case worth spelling out: the thing being
# checked being ABSENT (deleted, renamed, or commented out) must refuse, not
# approve. An earlier generation of guards in this repo approved exactly
# there, because absent read as "nothing to disagree with".
xval_script_tolerance() {
    local script="${1-}" lines count val
    [[ -n "$script" ]] || { echo "REFUSE: no script path given" >&2; return 2; }
    [[ -r "$script" ]] || { echo "REFUSE: cannot read '$script'" >&2; return 2; }

    # Leading-whitespace-tolerant, comment-intolerant: a line whose first
    # non-blank character is '#' is not an assignment.
    lines=$(grep -E '^[[:space:]]*TOLERANCE=' "$script" || true)
    if [[ -z "$lines" ]]; then
        echo "REFUSE: '$script' has no uncommented TOLERANCE= assignment -- the value this guard exists to check is absent, which is a refusal, not a pass" >&2
        return 2
    fi
    count=$(printf '%s\n' "$lines" | grep -c .)
    if [[ "$count" -ne 1 ]]; then
        echo "REFUSE: '$script' has $count TOLERANCE= assignments; which one gates is ambiguous" >&2
        return 2
    fi
    val=$(printf '%s\n' "$lines" | sed -n 's/^[[:space:]]*TOLERANCE=\([^[:space:]#]*\).*/\1/p')
    _xval_is_number "$val" || {
        echo "REFUSE: TOLERANCE in '$script' is not a plain number: '$val'" >&2
        return 2
    }
    echo "$val"
}

# xval_tolerance_matches_script SCRIPT_PATH [BIAS_PP SAMPLES K]
# Returns 0 only when the derivation and the shipped constant agree to the
# derivation's own precision (1 dp). Returns 2 and explains otherwise -- both
# for a genuine mismatch and for every "could not tell" case, which are the
# same disposition on purpose.
xval_tolerance_matches_script() {
    local script="${1-}" bias="${2-$XVAL_PREREG_BIAS_PP}"
    local samples="${3-$XVAL_PREREG_SAMPLES}" k="${4-$XVAL_PREREG_K}"
    local derived shipped
    derived=$(derive_xval_tolerance "$bias" "$samples" "$k") || return 2
    shipped=$(xval_script_tolerance "$script") || return 2
    # Both sides are guaranteed non-empty numbers by here: derive_* and
    # xval_script_tolerance each refuse before returning, so this comparison
    # can never be ""=="" .
    if [[ "$derived" != "$shipped" ]]; then
        echo "MISMATCH: '$script' ships TOLERANCE=$shipped but the pre-registered derivation (BIAS=$bias pp, N=$samples, K=$k) gives $derived pp" >&2
        return 2
    fi
    echo "$derived"
}

# extract_samples_in_window OUTPUT
# Echoes the N from cross_validate's "Samples in window: N" line.
# Echoes "?" when the line is missing or malformed -- NEVER 0. A parse
# failure that returned 0 would flow into
# refuse_unless_samples_sufficient as "thin capture" (correct by accident),
# but into any arithmetic as a real zero; "?" is not a number, so every
# consumer here refuses on it explicitly instead.
extract_samples_in_window() {
    local output="${1-}" line val
    line=$(printf '%s\n' "$output" | grep '^Samples in window:' | tail -1)
    [[ -n "$line" ]] || { echo "?"; return; }
    val=$(printf '%s\n' "$line" | sed -n 's/^Samples in window:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
    [[ -n "$val" ]] || { echo "?"; return; }
    echo "$val"
}

# refuse_unless_samples_sufficient OBSERVED_SAMPLES [MIN]
# Refuses a capture too thin for the comparison to mean anything. MIN
# defaults to the DERIVED floor (derive_xval_sample_floor, ~2587), NOT to
# XVAL_PREREG_SAMPLES -- see the long note beside the floor's derivation for
# why using the tolerance's own N here was wrong in two separate ways.
# Returns 0 when OBSERVED >= MIN, 2 otherwise AND when OBSERVED cannot be
# parsed -- an unknown sample count is never treated as sufficient.
refuse_unless_samples_sufficient() {
    local observed="${1-}" min="${2-}"
    if [[ -z "$min" ]]; then
        # A floor that cannot be derived must refuse, not fall back to a
        # permissive default: an underivable floor is a blind floor.
        min=$(derive_xval_sample_floor) || {
            echo "REFUSE: the sample floor could not be derived, so capture thickness cannot be judged" >&2
            return 2
        }
    fi
    _xval_is_number "$observed" || {
        echo "REFUSE: sample count is not a number ('$observed'); an unknown N is not evidence the derived bound applies" >&2
        return 2
    }
    _xval_is_number "$min" || { echo "REFUSE: minimum sample count is not a number ('$min')" >&2; return 2; }
    if [[ "${observed%%.*}" -lt "${min%%.*}" ]]; then
        echo "REFUSE: capture has ${observed} DB-time samples, below the derived floor of ${min} (the N at which this capture's sampling error would exceed the bound's budget by more than $(awk -v i="$XVAL_FLOOR_SE_INFLATION" 'BEGIN{printf "%.0f", (i-1)*100}')%); a capture this thin is structurally broken, not merely noisy" >&2
        return 2
    fi
    echo "$observed"
}
