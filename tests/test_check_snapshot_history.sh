#!/usr/bin/env bash
# test_check_snapshot_history.sh -- unit tests for
# scripts/check-snapshot-history.sh (tests/web_snapshots/VERSION RULE 3: a
# commit touching tests/web_snapshots/*.png must add or modify a
# tests/web_snapshots/history/*.md entry).
#
# No network, does not touch this repository's own git state or any real
# baseline PNG: every case builds a throwaway scratch git repo under
# `mktemp -d` (removed on exit) with its own tiny "master" branch and a
# fake tests/web_snapshots/ layout, then points
# scripts/check-snapshot-history.sh at it via its `[repo-dir]` argument.
#
# Wired into `make check` (scripts/check.sh) -- this guard is pure git, no
# Linux/BPF/PG dependency, so unlike most of tests/unit_tests.list it runs
# on the Mac, not just on the box.
#
# Usage: tests/test_check_snapshot_history.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUARD="$SCRIPT_DIR/../scripts/check-snapshot-history.sh"

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

expect_exit() {
    local want="$1" repo="$2" msg="$3"
    local out rc
    out=$("$GUARD" "$repo" 2>&1); rc=$?
    if [[ "$rc" == "$want" ]]; then
        report true "$msg (exit $rc)"
    else
        report false "$msg (wanted exit $want, got $rc; output: $out)"
    fi
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

repo="$tmpdir/repo"
mkdir -p "$repo/tests/web_snapshots/history"
git -C "$repo" init -q -b master
git -C "$repo" config user.email test@example.com
git -C "$repo" config user.name "Test"

echo "playwright==1.60.0" > "$repo/tests/web_snapshots/VERSION"
echo "fake-png-bytes-v1" > "$repo/tests/web_snapshots/table_events.png"
echo "# history index" > "$repo/tests/web_snapshots/history/README.md"
git -C "$repo" add -A
git -C "$repo" commit -q -m "initial baselines"

# Case 1: no tests/web_snapshots change at all -> passes trivially.
expect_exit 0 "$repo" "clean tree, nothing changed"

# Case 2 (RED): a baseline PNG changes with no history/ entry -- staged,
# not yet committed, exactly the "regenerate a baseline, forget the note"
# mistake this guard exists to catch.
echo "fake-png-bytes-v2" > "$repo/tests/web_snapshots/table_events.png"
git -C "$repo" add tests/web_snapshots/table_events.png
expect_exit 1 "$repo" "staged PNG change with no history entry is refused"

# Case 3 (GREEN): add the missing history entry (staged too) -> passes.
echo "2026-09-26 (#200 demo): table_events.png regenerated for the test." \
    > "$repo/tests/web_snapshots/history/2026-09-26-200-demo.md"
git -C "$repo" add tests/web_snapshots/history/2026-09-26-200-demo.md
expect_exit 0 "$repo" "same PNG change, now with a history entry, passes"

git -C "$repo" commit -q -m "regen table_events (#200)"

# Case 4: an UNRELATED file changes (e.g. VERSION's rules text) -- no PNG
# touched, must never require a history entry.
echo "playwright==1.61.0" > "$repo/tests/web_snapshots/VERSION"
git -C "$repo" add tests/web_snapshots/VERSION
expect_exit 0 "$repo" "non-PNG change under tests/web_snapshots never requires history"
git -C "$repo" commit -q -m "bump pin"

# Case 5: a brand-NEW baseline PNG (added, not modified) with no history
# entry is refused the same way a modified one is.
echo "fake-png-bytes-new" > "$repo/tests/web_snapshots/table_queries.png"
git -C "$repo" add tests/web_snapshots/table_queries.png
expect_exit 1 "$repo" "a newly added PNG with no history entry is refused"

# Case 6: the two-branches-different-baselines scenario this whole change
# exists for -- committed on top of master, diffed against it (the same
# "base = merge-base with origin/master" path make check exercises on a
# real feature branch). Reset case 5's staged-only add first.
git -C "$repo" reset -q --hard
git -C "$repo" checkout -q -b feature/regen-b
echo "fake-png-bytes-b" > "$repo/tests/web_snapshots/table_queries.png"
echo "2026-09-26 (#201 demo): table_queries.png regenerated for the test." \
    > "$repo/tests/web_snapshots/history/2026-09-26-201-demo.md"
git -C "$repo" add tests/web_snapshots/table_queries.png tests/web_snapshots/history/2026-09-26-201-demo.md
git -C "$repo" commit -q -m "regen table_queries (#201)"
expect_exit 0 "$repo" "a committed regen+history pair on a feature branch passes"

# Case 7 (RED, blocker-1 regression): a SECOND commit on the same feature
# branch regenerates a DIFFERENT baseline with no history entry. A
# whole-range "did ANY history file change anywhere in base..HEAD" check
# would wrongly pass here because commit 1 (#201, above) already added a
# history file elsewhere in the range; the guard must fail on THIS commit's
# own unaccounted PNG and must name it.
echo "fake-png-bytes-undocumented" > "$repo/tests/web_snapshots/table_events.png"
git -C "$repo" add tests/web_snapshots/table_events.png
git -C "$repo" commit -q -m "regen table_events, NO history entry (bug repro)"
out=$("$GUARD" "$repo" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "a 2nd commit's undocumented PNG is refused even though an earlier commit in the range has a history entry (exit 1)"
else
    report false "2nd undocumented commit refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "table_events.png" <<<"$out"; then
    report true "failure names the actually-unaccounted PNG (table_events.png)"
else
    report false "failure names the actually-unaccounted PNG (table_events.png); output: $out"
fi
if grep -q "table_queries.png" <<<"$out"; then
    report false "failure must not also blame the already-documented commit's PNG (table_queries.png); output: $out"
else
    report true "failure does not blame the already-documented commit's PNG (table_queries.png)"
fi

# Case 8 (GREEN): add the missing history entry in the SAME commit (amend)
# -> passes again, proving the fix path.
echo "2026-09-26 (#202 demo): table_events.png regenerated for the test (fix)." \
    > "$repo/tests/web_snapshots/history/2026-09-26-202-demo.md"
git -C "$repo" add tests/web_snapshots/history/2026-09-26-202-demo.md
git -C "$repo" commit -q --amend -m "regen table_events, WITH history entry (#202)"
expect_exit 0 "$repo" "amending the same commit to add the missing history entry fixes it"

# Case 9 (RED, blocker-2 regression): no comparable base exists at all (no
# origin remote, no local 'master' branch) -- must fail closed with a clear
# "no comparable base" message, never silently diff HEAD against itself and
# report OK (which would make an already-committed undocumented regen on
# this very branch invisible, exactly the shallow/single-branch-clone shape
# CI or an unusual checkout can take).
orphan="$tmpdir/orphan"
mkdir -p "$orphan/tests/web_snapshots/history"
git -C "$orphan" init -q -b trunk
git -C "$orphan" config user.email test@example.com
git -C "$orphan" config user.name "Test"
echo "fake-png-bytes" > "$orphan/tests/web_snapshots/only.png"
git -C "$orphan" add -A
git -C "$orphan" commit -q -m "only commit, no history entry, no master/origin"
out=$("$GUARD" "$orphan" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "no resolvable base fails closed (exit 1)"
else
    report false "no resolvable base fails closed (wanted exit 1, got $rc; output: $out)"
fi
if grep -qi "no comparable base" <<<"$out"; then
    report true "no-base failure says so explicitly (\"no comparable base\")"
else
    report false "no-base failure says so explicitly; output: $out"
fi

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
