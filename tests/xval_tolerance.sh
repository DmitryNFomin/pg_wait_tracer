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
# The bound above is only valid at or above the sample count its SE was
# computed from. A thinner capture has a larger SE, so reusing the bound
# would silently certify a looser quantity than the one derived.
# Returns 0 when OBSERVED >= MIN, 2 otherwise AND when OBSERVED cannot be
# parsed -- an unknown sample count is never treated as sufficient.
refuse_unless_samples_sufficient() {
    local observed="${1-}" min="${2-$XVAL_PREREG_SAMPLES}"
    _xval_is_number "$observed" || {
        echo "REFUSE: sample count is not a number ('$observed'); an unknown N is not evidence the derived bound applies" >&2
        return 2
    }
    _xval_is_number "$min" || { echo "REFUSE: minimum sample count is not a number ('$min')" >&2; return 2; }
    if [[ "${observed%%.*}" -lt "${min%%.*}" ]]; then
        echo "REFUSE: capture has ${observed} DB-time samples, below the ${min} the derived tolerance's sampling error was computed at; the bound does not apply to a capture this thin" >&2
        return 2
    fi
    echo "$observed"
}
