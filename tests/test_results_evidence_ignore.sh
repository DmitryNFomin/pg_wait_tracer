#!/usr/bin/env bash
# test_results_evidence_ignore.sh -- the tests/results/*/ class-ignore rule
# in .gitignore (added alongside #431/#433's gate-integrity work) must
# ignore a brand-new, never-seen evidence directory an agent invents, while
# never ignoring a tracked directory's content -- a tracked file that is
# also ignored is invisible to `git status` while git still tracks it,
# which is worse than the tree-hash drift this rule exists to fix.
#
# Runs against THIS repository's real .gitignore and git index (not a
# scratch repo: `git check-ignore` evaluates a path string against the
# repo's actual ignore rules without the path needing to exist on disk, and
# `git ls-files -i -c --exclude-standard` reads the real index) -- cheap,
# Mac-side, no box. NOT wired into `make check`/CI/any Makefile target,
# same reviewer-run-by-hand convention as
# tests/test_check_test_registration.sh; run it by hand before declaring
# READY on a change to this .gitignore rule.
#
# Usage: tests/test_results_evidence_ignore.sh (run from the repo root)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

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

# Control 1: a brand-new directory name nobody has used must be ignored AS
# A CLASS (the whole point -- the next agent's invented name needs no new
# .gitignore line).
if git check-ignore -q "tests/results/never-seen-$$-probe/file.json"; then
    report true "a brand-new, never-used tests/results/ directory name is ignored"
else
    report false "a brand-new tests/results/ directory name is ignored (git check-ignore did not match)"
fi

# Control 2: no TRACKED file is also ignored (the hard constraint: a
# blanket ignore that catches a tracked file is worse than the drift it
# fixes, since the file becomes invisible to `git status` while git keeps
# tracking it).
tracked_and_ignored=$(git ls-files -i -c --exclude-standard)
if [[ -z "$tracked_and_ignored" ]]; then
    report true "no tracked file under tests/results/ (or anywhere) is also ignored"
else
    report false "tracked-but-ignored files exist (must be none): $tracked_and_ignored"
fi

# Control 3: a NEW file inside an EXISTING, already-tracked evidence
# directory must still show up normally -- the class-ignore must not
# swallow real content just because it shares a parent with agent scratch.
if git check-ignore -q "tests/results/sampler-294-residual/a-new-file-that-should-be-tracked.txt"; then
    report false "a new file inside an existing tracked directory (sampler-294-residual) must NOT be ignored"
else
    report true "a new file inside an existing tracked directory (sampler-294-residual) is not ignored"
fi

# Control 4: the owner-facing documents the ignore rule must not touch.
for f in tests/results/HANDOVER-424.md tests/results/RELEASE-PLAN.md; do
    if git check-ignore -q "$f"; then
        report false "$f must not be ignored by the tests/results/*/ class rule (it is a FILE, not a directory)"
    else
        report true "$f is not ignored"
    fi
done

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
