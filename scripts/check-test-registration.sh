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
extract_unit_tests_list() {
    local content="$1"
    printf '%s\n' "$content" | sed -e 's/#.*$//' -e 's/[ \t]*$//' | sed '/^$/d'
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

    local missing
    missing=$(comm -23 <(printf '%s\n' "$base_entries" | sed '/^$/d' | sort -u) \
                        <(printf '%s\n' "$current_entries" | sed '/^$/d' | sort -u))
    if [[ -n "$missing" ]]; then
        echo "FAIL: test-registration guard: $label entries present at base ($base) but" >&2
        echo "  missing on this tree:" >&2
        printf '%s\n' "$missing" | sed 's/^/    /' >&2
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
