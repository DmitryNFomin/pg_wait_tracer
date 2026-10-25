#!/bin/bash
# test_summary_epoch_numbering.sh — the DORMANT summary accounting rule may
# not sit on the number a shipping format already claims.
#
# WHY THIS EXISTS. src/summary_writer.h spends ~200 lines arguing one point:
# `pgwt_summaries_window_current` compares only the VERSION NUMBER, so if a
# number ships while one of the accounting rules it names is not yet true,
# that number names two rules and no reader can tell the resulting files
# apart. The project's answer is a convention — "the commit that WIRES the
# producer is the one that sets the constant" — and until now the only thing
# enforcing it was that comment. It was already nearly broken once: the
# open-CPU rule held the number 9 while dormant, the next bump wanted 9 for
# a different set of rules, and the two would have collided silently (the
# rule was renumbered to v10 on 2026-10-25 for exactly that reason).
#
# WHAT IS CHECKED, and it is the WIRING that decides, not the prose:
#   - the dormant rule's acceptance test exists and its filename carries a
#     version number (tests/test_v<N>_open_cpu_wired.sh);
#   - while NO producer calls the dormant entry point, N must be STRICTLY
#     ABOVE PGWT_SUMMARY_VERSION, and the acceptance test must NOT be
#     registered in tests/run_all.sh (a dormant gate registered is a
#     permanent red, which is how the previous attempt broke box-check);
#   - once a producer DOES call it, N must EQUAL PGWT_SUMMARY_VERSION and
#     the acceptance test MUST be registered — the header's rule, checked
#     rather than remembered.
#
# REFUSES (exit 2) rather than passing whenever it cannot establish one of
# its inputs: no constant, no acceptance test, two of them, a filename with
# no number, a source tree with no .c files to scan, an unreadable runner, or
# a missing grep. A numbering check that cannot see the number approves every
# collision there will ever be — and "the thing being checked is absent" is
# never the same answer as "the thing being checked is correct".
#
# KNOWN LIMIT, stated so nobody reads more into a green: this checks the
# NUMBER against the WIRING. It does not read the accounting rules. Two
# rules that genuinely differ while sharing one number, both wired, is
# outside what any filename can detect.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
HDR="$REPO/src/summary_writer.h"
TESTDIR="$REPO/tests"
SRCDIR="$REPO/src"
RUNNER="$REPO/tests/run_all.sh"

# Scratch mode so the self-test below can drive this logic over fixtures:
#   $0 <header> <test-dir> <src-dir> <runner>
SCRATCH=0
if [[ $# -eq 4 ]]; then
    HDR="$1"; TESTDIR="$2"; SRCDIR="$3"; RUNNER="$4"; SCRATCH=1
fi

checks=0; fails=0
ok()  { checks=$((checks+1)); echo "  ok: $1"; }
bad() { checks=$((checks+1)); fails=$((fails+1)); echo "  FAIL: $1"; }
refuse() { echo "REFUSE: $1"; exit 2; }

# ── the tools this check cannot do without ──────────────────────────────
for tool in grep sed; do
    command -v "$tool" >/dev/null 2>&1 \
        || refuse "\`$tool\` is not executable -- with no way to read the"\
" sources there is nothing to compare, and a silent pass here would approve"\
" every numbering collision"
done

[[ -r "$HDR" ]] || refuse "cannot read $HDR -- the constant is the thing every
        other number is held to"
[[ -d "$TESTDIR" ]] || refuse "cannot read test directory $TESTDIR"
[[ -d "$SRCDIR" ]]  || refuse "cannot read source directory $SRCDIR"
[[ -r "$RUNNER" ]]  || refuse "cannot read $RUNNER -- registration is half of
        what this check decides"

# ── 1. the shipped constant ─────────────────────────────────────────────
V=$(sed -n 's/^#define[[:space:]]\{1,\}PGWT_SUMMARY_VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' \
        "$HDR" | head -1)
[[ -n "$V" ]] || refuse "no '#define PGWT_SUMMARY_VERSION <n>' in $HDR.
        Without it every number below would pass by default"

# ── 2. the dormant rule's acceptance test, and its number ───────────────
# Glob on `v*`, not `v[0-9]*`: a filename whose version is not a number must
# REFUSE, not silently fail to match and leave nothing to check.
shopt -s nullglob
cands=("$TESTDIR"/test_v*_open_cpu_wired.sh)
shopt -u nullglob
if [[ ${#cands[@]} -eq 0 ]]; then
    refuse "no tests/test_v<N>_open_cpu_wired.sh exists. The dormant rule's
        acceptance test being ABSENT is not the same as the numbering being
        right -- it is this check losing its only handle on the rule's number"
fi
if [[ ${#cands[@]} -gt 1 ]]; then
    refuse "${#cands[@]} files match tests/test_v<N>_open_cpu_wired.sh
        (${cands[*]##*/}). Two acceptance tests means two numbers claim the
        same rule and this check cannot say which is current"
fi
ACC="${cands[0]}"
ACC_BASE="$(basename "$ACC")"
W="${ACC_BASE#test_v}"; W="${W%%_open_cpu_wired.sh}"
case "$W" in
    ''|*[!0-9]*) refuse "$ACC_BASE carries no numeric version ('$W'). The
        filename IS the rule's number here, so an unparseable one leaves
        nothing to compare against PGWT_SUMMARY_VERSION" ;;
esac

# ── 3. the wiring state: does any producer call the dormant entry point? ─
# src/summary_writer.c is excluded on purpose -- it DEFINES the function and
# its own v8-signature wrapper delegates to it, so counting that file would
# report "wired" in every build that ever compiled.
shopt -s nullglob
srcs=()
for f in "$SRCDIR"/*.c; do
    [[ "$(basename "$f")" == "summary_writer.c" ]] && continue
    srcs+=("$f")
done
shopt -u nullglob
if [[ ${#srcs[@]} -eq 0 ]]; then
    refuse "no .c files in $SRCDIR other than summary_writer.c -- with no
        producer sources to scan, 'nothing calls it' would be an artefact of
        looking in the wrong place, not a finding"
fi
WIRED=0
if grep -l 'pgwt_summary_note_open_ex[[:space:]]*(' "${srcs[@]}" >/dev/null 2>&1; then
    WIRED=1
fi

# ── 4. registration, by ENTRY and never by mention ──────────────────────
# tests/run_all.sh names this file inside a comment explaining why it is NOT
# registered, so a bare `grep <name>` would report it registered forever --
# the same comment-vs-entry confusion scripts/check-runner-registration.py
# exists for. Only a LIVE_TESTS entry (`"name|runner|script"`) counts.
REGISTERED=0
if grep -Eq "^[[:space:]]*\"test_v${W}_open_cpu_wired\|" "$RUNNER"; then
    REGISTERED=1
fi

[[ $SCRATCH -eq 1 ]] || echo "=== test_summary_epoch_numbering \
(PGWT_SUMMARY_VERSION=$V, dormant rule=v$W, wired=$WIRED, registered=$REGISTERED) ==="

# ── 5. the verdict ──────────────────────────────────────────────────────
if [[ "$WIRED" -eq 0 ]]; then
    if [[ "$W" -gt "$V" ]]; then
        ok "no producer calls pgwt_summary_note_open_ex, and the dormant rule's number ($W) is above the shipped constant ($V)"
    else
        bad "no producer calls pgwt_summary_note_open_ex, yet the dormant rule's number ($W) is not above the shipped constant ($V) -- a shipped $V would name both the rules it implements AND a rule nothing implements, and pgwt_summaries_window_current compares only the number"
    fi
    if [[ "$REGISTERED" -eq 0 ]]; then
        ok "the dormant rule's acceptance test is not registered in run_all.sh"
    else
        bad "$ACC_BASE IS registered in run_all.sh while the producer is dormant -- it asserts a non-zero counter nothing can move, so the live tier is permanently red (this is what broke box-check before fc7d7f5)"
    fi
else
    if [[ "$W" -eq "$V" ]]; then
        ok "a producer calls pgwt_summary_note_open_ex and the constant ($V) matches the rule's number ($W)"
    else
        bad "a producer calls pgwt_summary_note_open_ex but the constant is $V while the rule is numbered $W -- the wiring commit must set the constant in the SAME commit (src/summary_writer.h states why: otherwise one number names two accounting rules)"
    fi
    if [[ "$REGISTERED" -eq 1 ]]; then
        ok "$ACC_BASE is registered in run_all.sh now that the producer is wired"
    else
        bad "a producer calls pgwt_summary_note_open_ex but $ACC_BASE is not a LIVE_TESTS entry in run_all.sh -- the acceptance test for the one failure that 'still looks green' would never run"
    fi
fi

if [[ $SCRATCH -eq 1 ]]; then
    [[ "$fails" -eq 0 ]] || exit 1
    exit 0
fi

# ── self-test: every way this check could stop checking ─────────────────
echo "--- self-test ---"
self="$REPO/tests/test_summary_epoch_numbering.sh"
scratch=$(mktemp -d) || refuse "no scratch dir"
trap 'rm -rf "$scratch"' EXIT

# mk <constant> <acc-filename-or-NONE-or-TWO> <wired:0|1> <reg:0|1|COMMENT>
mk() {
    rm -rf "$scratch/t" "$scratch/s"
    mkdir -p "$scratch/t" "$scratch/s"
    printf '#define PGWT_SUMMARY_VERSION  %s\n' "$1" > "$scratch/h.h"
    case "$2" in
        NONE) : ;;
        TWO)  : > "$scratch/t/test_v9_open_cpu_wired.sh"
              : > "$scratch/t/test_v10_open_cpu_wired.sh" ;;
        *)    : > "$scratch/t/test_v$2_open_cpu_wired.sh" ;;
    esac
    # summary_writer.c always "calls" it (it defines the wrapper) and must be
    # excluded; map_reader.c is the real producer.
    printf 'pgwt_summary_note_open_ex(w, 1, 2);\n' > "$scratch/s/summary_writer.c"
    if [[ "$3" == "1" ]]; then
        printf 'pgwt_summary_note_open_ex(d->summary_writer, p, we);\n' \
            > "$scratch/s/map_reader.c"
    else
        printf 'pgwt_summary_note_open(d->summary_writer, p, we);\n' \
            > "$scratch/s/map_reader.c"
    fi
    case "$4" in
        1) printf 'LIVE_TESTS=(\n    "test_v%s_open_cpu_wired|bash|test_v%s_open_cpu_wired.sh"\n)\n' "$2" "$2" > "$scratch/r.sh" ;;
        COMMENT) printf 'LIVE_TESTS=(\n    # NOT registered: test_v%s_open_cpu_wired.sh -- dormant\n)\n' "$2" > "$scratch/r.sh" ;;
        *) printf 'LIVE_TESTS=(\n)\n' > "$scratch/r.sh" ;;
    esac
}
leg() {  # leg <name> <want-rc>
    "$self" "$scratch/h.h" "$scratch/t" "$scratch/s" "$scratch/r.sh" \
        >/dev/null 2>&1
    local got=$?
    if [[ "$got" -eq "$2" ]]; then ok "$1 (rc=$got)"
    else bad "$1 -- rc=$got, want $2"; fi
}

# --- the two legitimate states ---
mk 9 10 0 0; leg "dormant rule above the constant, unregistered, is CLEAN" 0
mk 9 9  1 1; leg "wired rule equal to the constant, registered, is CLEAN" 0

# --- true positives: the collision this check exists for ---
mk 9 9  0 0; leg "dormant rule ON the constant is RED (the 2026-10-25 collision)" 1
mk 9 8  0 0; leg "dormant rule BELOW the constant is RED" 1
mk 9 10 0 1; leg "dormant rule REGISTERED is RED (permanent live-tier red)" 1
mk 9 10 1 0; leg "wired producer with the constant left behind is RED" 1
mk 9 9  1 0; leg "wired producer with an UNREGISTERED acceptance test is RED" 1

# --- false-negative suite: each way the check could stop seeing ---
mk 9 10 0 COMMENT
leg "named only in a run_all COMMENT still counts as unregistered" 0
mk 9 9 0 COMMENT
leg "...and a comment does not rescue a collision from being RED" 1

mk 9 10 0 0; printf 'no define at all\n' > "$scratch/h.h"
leg "a header with no PGWT_SUMMARY_VERSION REFUSES, never approves" 2

mk 9 10 0 0; rm -f "$scratch/h.h"
leg "an unreadable header REFUSES" 2

mk 9 NONE 0 0
leg "NO acceptance test at all REFUSES (absent is not correct)" 2

mk 9 TWO 0 0
leg "TWO acceptance tests REFUSE (two numbers for one rule)" 2

mk 9 X 0 0
leg "an unparseable version in the filename REFUSES" 2

mk 9 10 0 0; rm -f "$scratch/s"/*.c
leg "a source tree with no producer .c files REFUSES" 2

mk 9 10 0 0; rm -f "$scratch/s"/map_reader.c
leg "only summary_writer.c present REFUSES (it is the excluded file)" 2

mk 9 10 0 0; rm -f "$scratch/r.sh"
leg "an unreadable runner REFUSES" 2

mk 9 10 0 0; rm -rf "$scratch/t"
leg "a missing test directory REFUSES" 2

# a dependency that cannot be executed at all (the 126/127 case)
mk 9 10 0 0
fakebin=$(mktemp -d)
printf '#!/bin/sh\nexit 127\n' > "$fakebin/grep"; chmod -x "$fakebin/grep"
PATH="$fakebin" "$self" "$scratch/h.h" "$scratch/t" "$scratch/s" \
    "$scratch/r.sh" >/dev/null 2>&1
rc=$?
rm -rf "$fakebin"
if [[ "$rc" -eq 2 ]]; then ok "a non-executable grep REFUSES (rc=2)"
else bad "a non-executable grep did not REFUSE -- rc=$rc, want 2"; fi

echo ""
echo "$checks checks, $fails failures"
[[ "$fails" -eq 0 ]] || exit 1
exit 0
