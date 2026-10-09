#!/usr/bin/env bash
# test_check_test_registration.sh -- unit tests for
# scripts/check-test-registration.sh (a rebase must not silently drop a test
# registration from tests/unit_tests.list or tests/Makefile's TESTS list).
#
# No network, does not touch this repository's own git state: every case
# builds a throwaway scratch git repo under `mktemp -d` (removed on exit)
# with its own tiny "master" branch and fake tests/unit_tests.list /
# tests/Makefile files, then points scripts/check-test-registration.sh at it
# via its `[repo-dir]` argument.
#
# Wired into `make check` (scripts/check.sh) -- this guard is pure git, no
# Linux/BPF/PG dependency, so unlike most of tests/unit_tests.list it runs
# on the Mac, not just on the box.
#
# Usage: tests/test_check_test_registration.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUARD="$SCRIPT_DIR/../scripts/check-test-registration.sh"

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

write_fixture() {
    # write_fixture <repo> <list-entries...> -- writes tests/unit_tests.list
    # and a minimal tests/Makefile whose TESTS = line lists the same names,
    # split across two continuation lines like the real file.
    local repo="$1"; shift
    {
        echo "# unit_tests.list fixture"
        for t in "$@"; do echo "$t"; done
    } > "$repo/tests/unit_tests.list"
    {
        echo "CC ?= gcc"
        echo "TESTS = \\"
        local n=0
        for t in "$@"; do
            n=$((n + 1))
            if [[ $n -eq $# ]]; then
                echo "    $t"
            else
                echo "    $t \\"
            fi
        done
        echo "all: \$(TESTS)"
    } > "$repo/tests/Makefile"
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

repo="$tmpdir/repo"
mkdir -p "$repo/tests"
git -C "$repo" init -q -b master
git -C "$repo" config user.email test@example.com
git -C "$repo" config user.name "Test"

write_fixture "$repo" test_a test_b test_c
git -C "$repo" add -A
git -C "$repo" commit -q -m "initial test registrations"

# Case 1: clean tree, nothing changed -> passes.
expect_exit 0 "$repo" "clean tree, nothing changed"

# Case 2 (RED): on a feature branch, a "rebase" drops test_b from BOTH
# files (the test_exec_index incident: the target/line vanished, nothing
# else changed) -- staged, not yet committed.
git -C "$repo" checkout -q -b feature/drop
write_fixture "$repo" test_a test_c
git -C "$repo" add -A
out=$("$GUARD" "$repo" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "a dropped entry (test_b) is refused (exit 1)"
else
    report false "a dropped entry (test_b) is refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "test_b" <<<"$out"; then
    report true "failure names the actually-dropped entry (test_b)"
else
    report false "failure names the actually-dropped entry (test_b); output: $out"
fi

# Case 3 (GREEN): restore the dropped entry -> passes again.
write_fixture "$repo" test_a test_b test_c
git -C "$repo" add -A
expect_exit 0 "$repo" "restoring the dropped entry fixes it"

# Case 4 (GREEN): append-only is fine -- a rebase may ADD entries.
write_fixture "$repo" test_a test_b test_c test_d
git -C "$repo" add -A
git -C "$repo" commit -q -m "add test_d"
expect_exit 0 "$repo" "adding a new entry (append-only) passes"

# Case 5 (RED): drop ONLY from tests/Makefile's TESTS list, leave
# unit_tests.list untouched -- the two files are checked independently.
{
    echo "# unit_tests.list fixture"
    echo "test_a"
    echo "test_b"
    echo "test_c"
    echo "test_d"
} > "$repo/tests/unit_tests.list"
{
    echo "CC ?= gcc"
    echo "TESTS = \\"
    echo "    test_a \\"
    echo "    test_c \\"
    echo "    test_d"
    echo "all: \$(TESTS)"
} > "$repo/tests/Makefile"
git -C "$repo" add -A
out=$("$GUARD" "$repo" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "a Makefile-only drop (test_b from TESTS) is refused (exit 1)"
else
    report false "a Makefile-only drop refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "Makefile TESTS" <<<"$out" && grep -q "test_b" <<<"$out"; then
    report true "failure names the file (Makefile TESTS) and the dropped entry (test_b)"
else
    report false "failure names the file and entry; output: $out"
fi
git -C "$repo" checkout -q -- tests/Makefile tests/unit_tests.list

# Case 6 (RED, parser self-check): the base commit's tests/Makefile has a
# real, non-empty TESTS assignment, but on the branch the TESTS variable is
# spelled differently (format drift) so this guard's own parser would find
# zero entries there. Comparing an honestly-empty "nothing changed" list
# against this would wrongly PASS even though every test is, from the
# parser's point of view, gone -- exactly the "unparseable field must
# refuse, not approve" failure mode. Here we flip it: corrupt the BASE
# commit's format instead, so the base-side parse yields zero while the
# base file is clearly non-empty -- must refuse rather than silently
# treating "parsed nothing" as "nothing required".
base_bad="$tmpdir/base_bad"
mkdir -p "$base_bad/tests"
git -C "$base_bad" init -q -b master
git -C "$base_bad" config user.email test@example.com
git -C "$base_bad" config user.name "Test"
{
    echo "# unit_tests.list fixture"
    echo "test_a"
} > "$base_bad/tests/unit_tests.list"
{
    echo "CC ?= gcc"
    echo "TESTS_RENAMED = \\"
    echo "    test_a"
    echo "all: \$(TESTS_RENAMED)"
} > "$base_bad/tests/Makefile"
git -C "$base_bad" add -A
git -C "$base_bad" commit -q -m "base with a TESTS variable this parser cannot read"
git -C "$base_bad" checkout -q -b feature/anything
write_fixture "$base_bad" test_a
git -C "$base_bad" add -A
git -C "$base_bad" commit -q -m "feature branch, normally-formatted Makefile"
out=$("$GUARD" "$base_bad" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "an unparseable base TESTS format is refused rather than silently passed (exit 1)"
else
    report false "unparseable base TESTS format refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -qi "could not parse" <<<"$out"; then
    report true "unparseable-base failure says so explicitly (\"could not parse\")"
else
    report false "unparseable-base failure says so explicitly; output: $out"
fi

# Case 7 (RED, no resolvable base): no origin remote, no local 'master'
# branch -- must fail closed with a clear "no comparable base" message,
# never silently diff HEAD against itself (which would make a dropped entry
# on this very branch invisible).
orphan="$tmpdir/orphan"
mkdir -p "$orphan/tests"
git -C "$orphan" init -q -b trunk
git -C "$orphan" config user.email test@example.com
git -C "$orphan" config user.name "Test"
write_fixture "$orphan" test_only
git -C "$orphan" add -A
git -C "$orphan" commit -q -m "only commit, no master/origin"
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

# Case 8: the files are simply absent at the base (e.g. this guard runs on
# a repo whose history predates tests/unit_tests.list existing at all) --
# "absent rather than wrong": must not crash, and since nothing was
# registered yet there is nothing to have dropped -> passes.
no_files_base="$tmpdir/no_files_base"
mkdir -p "$no_files_base/tests"
git -C "$no_files_base" init -q -b master
git -C "$no_files_base" config user.email test@example.com
git -C "$no_files_base" config user.name "Test"
echo "placeholder" > "$no_files_base/tests/README"
git -C "$no_files_base" add -A
git -C "$no_files_base" commit -q -m "repo before unit_tests.list/Makefile existed"
git -C "$no_files_base" checkout -q -b feature/introduce
write_fixture "$no_files_base" test_new
git -C "$no_files_base" add -A
git -C "$no_files_base" commit -q -m "introduce the test-registration files"
expect_exit 0 "$no_files_base" "base predating these files entirely still passes (nothing to drop)"

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
