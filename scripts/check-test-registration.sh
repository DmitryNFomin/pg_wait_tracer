#!/usr/bin/env bash
# check-test-registration.sh — refuse a rebase that silently drops a test
# registration. This already happened: Phase 1's rebase resolved a conflict
# in tests/Makefile and left master's test_exec_index recipe WITHOUT its
# command line — the target existed and did nothing. It was caught only
# because the implementer rebuilt and ran the tests by hand instead of
# trusting the merge.
#
# Invariant: tests/unit_tests.list and tests/Makefile's TESTS list are
# APPEND-ONLY across a rebase. Every entry present at the merge-base must
# still be present on the branch; a rebase may add entries, never lose one.
# This does NOT check that a name still builds/runs (make -C tests check
# does that) — only that nobody silently resolved a conflict by deleting a
# line.
#
# Base resolution is FAIL-CLOSED, same reasoning as
# scripts/check-snapshot-history.sh: if neither origin/master nor a local
# master branch resolves a merge-base, this refuses rather than quietly
# comparing HEAD to itself (which would make every dropped entry on an
# already-committed branch invisible).
#
# Checks the CURRENT WORKING TREE (not HEAD), so uncommitted edits are
# covered the same as committed ones -- this is what `make check` and a
# reviewer's working copy actually see.
#
# Usage: scripts/check-test-registration.sh [repo-dir]
#   repo-dir defaults to this script's own repository; a test harness passes
#   a scratch git repo to exercise the logic in isolation (see
#   tests/test_check_test_registration.sh).
#
# A guard that cannot run under the invoking shell must refuse, not silently
# approve (issue #431). This script requires bash: the comparison below uses
# `<(...)` process substitution, `[[ ]]` and function-local state, none of
# which a plain `sh` reliably provides. Two distinct ways that bites:
#   - real dash (the Linux gate boxes' /bin/sh) hard syntax-errors on `<(...)`
#     and aborts outright -- noisy, but at least it does not approve;
#   - bash invoked AS "sh" (e.g. macOS's /bin/sh, which IS bash, so
#     $BASH_VERSION is still set) resolves `<(...)` to a failed command
#     substitution whose nonzero exit status nothing downstream used to
#     check, so the comparison silently became "nothing missing" -- a
#     registration genuinely removed from tests/unit_tests.list, run through
#     `sh scripts/check-test-registration.sh`, printed OK and exited 0.
# The $BASH_VERSION check below closes the first case (dash never sets it);
# the explicit exit-status check after the `comm` calls further down closes
# the second (same bash binary, process substitution still unusable). Pure
# POSIX /bin/sh syntax ONLY until that `exit`, so this check itself can run
# under the very shell it may need to reject.
if [ -z "${BASH_VERSION:-}" ]; then
    echo "FAIL: test-registration guard: not running under bash (\$BASH_VERSION is" >&2
    echo "  unset) -- refusing to approve blind. Re-run as" >&2
    echo "  './scripts/check-test-registration.sh' or" >&2
    echo "  'bash scripts/check-test-registration.sh', never" >&2
    echo "  'sh scripts/check-test-registration.sh': this guard's comparison relies" >&2
    echo "  on bash's <(...) process substitution, which plain sh does not reliably" >&2
    echo "  provide (issue #431)." >&2
    exit 1
fi

set -uo pipefail

repo="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$repo" || { echo "FAIL: test-registration guard: no such repo dir: $repo" >&2; exit 1; }

resolve_base() {
    local ref
    for ref in origin/master master; do
        if git rev-parse --verify -q "$ref" >/dev/null; then
            if git merge-base HEAD "$ref" 2>/dev/null; then
                return 0
            fi
        fi
    done
    return 1
}

base=$(resolve_base) || {
    echo "FAIL: test-registration guard: no comparable base ref (origin/master or" >&2
    echo "master) resolvable from HEAD ($(git rev-parse HEAD 2>/dev/null || echo '?')) —" >&2
    echo "refusing to approve blind. Fetch origin/master (or ensure a local master" >&2
    echo "branch exists) before running this guard." >&2
    exit 1
}

# tests/unit_tests.list entries: one name per line, '#' comments and blank
# lines stripped, trailing whitespace trimmed.
#
# NOT `sed -e 's/[ \t]*$//'`: that reads as a POSIX BASIC regex bracket
# expression, where `\t` is the two literal characters backslash and `t`,
# not a tab escape -- GNU sed accepts `\t` as a GNU extension, but macOS's
# shipped /usr/bin/sed (BSD sed, the ONLY sed this guard's documented
# "Mac-side, seconds, no box" mode ever runs under, and this Mac has no
# `gsed`) takes it literally and silently strips trailing runs of the
# characters ' ', '\', 't' -- e.g. "test_bucket" -> "test_bucke". Found by
# review, reproduced directly: `echo test_bucket | sed -e 's/[ \t]*$//'`
# mangles 5 of the 52 real entries in tests/unit_tests.list on this Mac.
# `[[:space:]]` is the POSIX character class both seds actually support.
extract_unit_tests_list() {
    local content="$1"
    printf '%s\n' "$content" | sed -e 's/#.*$//' -e 's/[[:space:]]*$//' | sed '/^$/d'
}

# tests/Makefile's TESTS = ... \ ... multi-line assignment, token per line.
extract_makefile_tests() {
    local content="$1"
    printf '%s\n' "$content" | awk '
        started {
            line = $0
            cont = (line ~ /\\$/)
            gsub(/\\$/, "", line)
            print line
            if (!cont) { started = 0 }
            next
        }
        /^TESTS[ \t]*=/ {
            started = 1
            line = $0
            sub(/^TESTS[ \t]*=[ \t]*/, "", line)
            cont = (line ~ /\\$/)
            gsub(/\\$/, "", line)
            print line
            if (!cont) { started = 0 }
        }
    ' | tr -s ' \t' '\n' | sed '/^$/d'
}

fail=0
missing_total=0

# $1 = human label, $2 = base entries (newline-separated), $3 = current
# entries (newline-separated), $4 = base file content (to tell "legitimately
# nothing registered" from "parser could not read this file at all").
check_one() {
    local label="$1" base_entries="$2" current_entries="$3" base_raw="$4"
    local base_count
    base_count=$(printf '%s\n' "$base_entries" | sed '/^$/d' | wc -l | tr -d ' ')

    # Parser self-check: the base file has real content but our extraction
    # found nothing registered at all. A real repo's base commit always has
    # a non-empty TESTS list / unit_tests.list, so zero here means the
    # parser could not understand the format -- NOT that nothing is
    # registered. Silently treating that as "nothing to check" would turn
    # every real drop invisible (comparing empty against empty always
    # passes). Refuse instead.
    if [[ -n "$(printf '%s' "$base_raw" | tr -d '[:space:]')" && "$base_count" -eq 0 ]]; then
        echo "FAIL: test-registration guard: could not parse any $label entries at base" >&2
        echo "  ($base) even though the file is non-empty -- refusing to approve blind" >&2
        echo "  (the parser format may have changed; this guard needs updating, not bypassing)." >&2
        fail=1
        return
    fi

    # Sort each side as a PLAIN pipeline first (no process substitution yet),
    # so `set -o pipefail` (active from the top of this file) gives an exit
    # status for the actual producer -- `sort` -- not just for `comm`
    # consuming whatever two streams happen to land in front of it. This
    # closes the identical root cause one step further upstream: a failed
    # producer (sort missing, or a `sort` that exits non-zero having written
    # nothing) feeding `comm` through `<(...)` used to present as an EMPTY
    # but structurally valid stream on both sides -- `comm -23 <empty>
    # <empty>` succeeds and prints nothing, so a genuinely deleted
    # registration read as "nothing missing" even though the comparison
    # never actually ran. Checking only comm's own $? (as a prior version of
    # this guard did) cannot see that: comm has no way to know its input
    # pipe was fed by a producer that failed. Reproduced directly: shim a
    # `sort` on PATH that exits 1 with no output, delete a real registration,
    # run this guard -- pre-fix it printed OK and exited 0.
    local base_sorted current_sorted rc
    base_sorted=$(printf '%s\n' "$base_entries" | sed '/^$/d' | sort -u)
    rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "FAIL: test-registration guard: could not sort $label entries at base" >&2
        echo "  ($base) (sed/sort exited $rc) -- refusing to approve blind. A failed" >&2
        echo "  sort/sed here would otherwise present as an empty-but-valid stream to" >&2
        echo "  the comparison below, silently hiding every entry that was there." >&2
        fail=1
        return
    fi
    current_sorted=$(printf '%s\n' "$current_entries" | sed '/^$/d' | sort -u)
    rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "FAIL: test-registration guard: could not sort $label entries on this tree" >&2
        echo "  (sed/sort exited $rc) -- refusing to approve blind. A failed sort/sed" >&2
        echo "  here would otherwise present as an empty-but-valid stream to the" >&2
        echo "  comparison below, silently hiding every entry that was dropped." >&2
        fail=1
        return
    fi

    # Both sides are now known-good, already-sorted strings; feeding them to
    # `comm` via `printf | <(...)` cannot reintroduce the same failure mode
    # -- printf of a literal string does not fail.
    local missing
    missing=$(comm -23 <(printf '%s\n' "$base_sorted") <(printf '%s\n' "$current_sorted"))
    rc=$?
    # The $BASH_VERSION check at the top of this file already refuses a shell
    # that never had process substitution (dash). This check is the second
    # line of defense: it catches the same feature failing at RUNTIME inside
    # a shell that otherwise looks like bash (e.g. bash invoked as "sh" on
    # macOS, where $BASH_VERSION stays set but `<(...)` resolves to a broken
    # command substitution instead of a syntax error) -- without it, `missing`
    # silently ends up empty and this guard reports "nothing missing" for a
    # comparison that never actually ran. Reproduced directly: remove a real
    # entry from tests/unit_tests.list, run `sh scripts/check-test-registration.sh`
    # pre-fix -- it printed OK and exited 0.
    if [[ $rc -ne 0 ]]; then
        echo "FAIL: test-registration guard: could not compare $label entries (the" >&2
        echo "  comm/process-substitution comparison exited $rc) -- refusing to approve blind." >&2
        echo "  This usually means the invoking shell does not support bash's <(...) process" >&2
        echo "  substitution; re-run with './scripts/check-test-registration.sh' or" >&2
        echo "  'bash scripts/check-test-registration.sh', not 'sh scripts/check-test-registration.sh'." >&2
        fail=1
        return
    fi
    if [[ -n "$missing" ]]; then
        echo "FAIL: test-registration guard: $label entries present at base ($base) but" >&2
        echo "  missing on this tree:" >&2
        printf '%s\n' "$missing" | sed 's/^/    /' >&2
        echo "  This guard matches by exact name, so an intentional rename or removal" >&2
        echo "  reads as a drop too -- that is expected, not a bug. If the name above" >&2
        echo "  was deliberately renamed or retired (not silently lost in a merge), say" >&2
        echo "  so in the PR body and move on; this guard cannot tell the difference" >&2
        echo "  from the outside, only a human reviewing the diff can." >&2
        fail=1
        missing_total=$((missing_total + $(printf '%s\n' "$missing" | sed '/^$/d' | wc -l)))
    fi
}

base_list_raw=$(git show "$base:tests/unit_tests.list" 2>/dev/null || true)
base_mk_raw=$(git show "$base:tests/Makefile" 2>/dev/null || true)
cur_list_raw=$(cat tests/unit_tests.list 2>/dev/null || true)
cur_mk_raw=$(cat tests/Makefile 2>/dev/null || true)

check_one "tests/unit_tests.list" \
    "$(extract_unit_tests_list "$base_list_raw")" \
    "$(extract_unit_tests_list "$cur_list_raw")" \
    "$base_list_raw"

check_one "tests/Makefile TESTS" \
    "$(extract_makefile_tests "$base_mk_raw")" \
    "$(extract_makefile_tests "$cur_mk_raw")" \
    "$base_mk_raw"

if [[ $fail -ne 0 ]]; then
    echo "A rebase/merge must not silently drop a test registration -- restore the" >&2
    echo "missing entry/entries listed above (see tests/unit_tests.list's own header" >&2
    echo "and tests/Makefile's TESTS variable)." >&2
    exit 1
fi

echo "OK: test-registration guard (base $base) — append-only invariant holds"
exit 0
