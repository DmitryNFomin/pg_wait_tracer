#!/usr/bin/env bash
# test_check_runner_registration.sh -- unit tests for
# scripts/check-runner-registration.sh / .py (issue #433: reconcile
# tests/run_all.sh's and tests/ci_smoke.sh's actual membership).
#
# No network, does not touch this repository's own git state: every case
# builds a throwaway scratch directory under `mktemp -d` (removed on exit)
# with its own tiny tests/run_all.sh, tests/ci_smoke.sh and (where needed)
# tests/runner_registration_exemptions.list, then points
# scripts/check-runner-registration.sh at it via its `[repo-dir]` argument.
#
# NOT wired into `make check`, CI, or any Makefile target -- same
# reviewer-run-by-hand convention as tests/test_check_test_registration.sh.
#
# Usage: tests/test_check_runner_registration.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUARD="$SCRIPT_DIR/../scripts/check-runner-registration.sh"

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

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# write_reconciled <repo> -- a minimal but realistic pair of runners where
# test_a is invoked by both (directly in ci_smoke.sh, via the LIVE_TESTS
# array in run_all.sh), test_b is a direct-invocation-only run_all.sh entry
# (legitimately absent from the narrow ci_smoke.sh, no exemption needed),
# and a bare mention of "test_ghost.py" inside a FULL-LINE COMMENT in
# run_all.sh does NOT count as a registration (the instance-3 shape).
write_reconciled() {
    local repo="$1"
    mkdir -p "$repo/tests"
    cat > "$repo/tests/run_all.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_test() { :; }
run_test "test_b" python3 "$SCRIPT_DIR/test_b.py"
# This line LOOKS like a real invocation but is a disabled, commented-out
# one (the instance-3 shape: a naive grep for the invocation pattern finds
# this and wrongly counts it as registered; this guard must not):
#     python3 "$SCRIPT_DIR/test_ghost.py"
LIVE_TESTS=(
    "test_a|python3|test_a.py"
)
for entry in "${LIVE_TESTS[@]}"; do
    IFS='|' read -r name runner file <<< "$entry"
    case "$runner" in
        python3) run_test "$name" python3 "$SCRIPT_DIR/$file" ;;
    esac
done
EOF
    cat > "$repo/tests/ci_smoke.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_section() { :; }
run_section "a" python3 "$SCRIPT_DIR/test_a.py"
EOF
}

# Case 1 (GREEN): reconciled -- test_a is in both, test_b is run_all-only
# (fine), test_ghost is only a comment mention (correctly not "registered").
repo1="$tmpdir/case1"
write_reconciled "$repo1"
out=$("$GUARD" "$repo1" 2>&1); rc=$?
if [[ "$rc" == "0" ]]; then
    report true "reconciled membership passes (exit 0)"
else
    report false "reconciled membership passes (wanted exit 0, got $rc; output: $out)"
fi

# Case 2 (RED, instance 3 exactly -- "the case a grep implementation gets
# wrong, so it is the one that matters most"): ci_smoke.sh really invokes
# test_ghost.py; run_all.sh only ever MENTIONS it inside a full-line
# comment. A naive grep for the filename would find it in run_all.sh and
# wrongly call it registered; this guard must not.
repo2="$tmpdir/case2"
write_reconciled "$repo2"
cat >> "$repo2/tests/ci_smoke.sh" <<'EOF'
run_section "ghost" python3 "$SCRIPT_DIR/test_ghost.py"
EOF
out=$("$GUARD" "$repo2" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "ci_smoke-only test_ghost.py (run_all mentions it only in a comment) is refused (exit 1)"
else
    report false "test_ghost.py gap refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "test_ghost.py" <<<"$out"; then
    report true "failure names the undetected-registration file (test_ghost.py)"
else
    report false "failure names test_ghost.py; output: $out"
fi
if grep -q "test_b.py" <<<"$out" || grep -q "test_a.py" <<<"$out"; then
    report false "failure must name ONLY the real gap, not the legitimately-reconciled files; output: $out"
else
    report true "failure does not also flag the already-reconciled test_a.py / run_all-only test_b.py"
fi

# Case 3 (RED, "delete a registration from run_all.sh"): test_a was in both;
# drop it from run_all.sh's LIVE_TESTS array entirely (ci_smoke.sh still has
# it) -- the guard must notice and name test_a.py.
repo3="$tmpdir/case3"
write_reconciled "$repo3"
cat > "$repo3/tests/run_all.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_test() { :; }
run_test "test_b" python3 "$SCRIPT_DIR/test_b.py"
LIVE_TESTS=()
for entry in "${LIVE_TESTS[@]:-}"; do
    IFS='|' read -r name runner file <<< "$entry"
    case "$runner" in
        python3) run_test "$name" python3 "$SCRIPT_DIR/$file" ;;
    esac
done
EOF
out=$("$GUARD" "$repo3" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "test_a.py dropped from run_all.sh's LIVE_TESTS is refused (exit 1)"
else
    report false "dropped test_a.py refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "test_a.py" <<<"$out"; then
    report true "failure names the dropped entry (test_a.py)"
else
    report false "failure names test_a.py; output: $out"
fi

# Case 4 (RED, "move a test from one runner to the other"): test_b used to
# be run_all-only (fine); move it so ONLY ci_smoke.sh invokes it now. The
# guard must notice test_b.py just as it would any other ci-only file.
repo4="$tmpdir/case4"
write_reconciled "$repo4"
cat > "$repo4/tests/run_all.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_test() { :; }
LIVE_TESTS=(
    "test_a|python3|test_a.py"
)
for entry in "${LIVE_TESTS[@]}"; do
    IFS='|' read -r name runner file <<< "$entry"
    case "$runner" in
        python3) run_test "$name" python3 "$SCRIPT_DIR/$file" ;;
    esac
done
EOF
cat > "$repo4/tests/ci_smoke.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_section() { :; }
run_section "a" python3 "$SCRIPT_DIR/test_a.py"
run_section "b, moved here" python3 "$SCRIPT_DIR/test_b.py"
EOF
out=$("$GUARD" "$repo4" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "test_b.py moved to ci_smoke-only is refused (exit 1)"
else
    report false "moved test_b.py refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -q "test_b.py" <<<"$out"; then
    report true "failure names the moved entry (test_b.py)"
else
    report false "failure names test_b.py; output: $out"
fi

# Case 5 (GREEN): the SAME ci-only gap as Case 2, but now documented in
# tests/runner_registration_exemptions.list with a reason -- must pass, and
# the OK message must say how many exemptions are in effect.
repo5="$tmpdir/case5"
write_reconciled "$repo5"
cat >> "$repo5/tests/ci_smoke.sh" <<'EOF'
run_section "ghost" python3 "$SCRIPT_DIR/test_ghost.py"
EOF
cat > "$repo5/tests/runner_registration_exemptions.list" <<'EOF'
# test fixture exemption
test_ghost.py|deliberately CI-only for this fixture
EOF
out=$("$GUARD" "$repo5" 2>&1); rc=$?
if [[ "$rc" == "0" ]]; then
    report true "a documented exemption (test_ghost.py) passes (exit 0)"
else
    report false "documented exemption passes (wanted exit 0, got $rc; output: $out)"
fi
if grep -q "1 listed" <<<"$out"; then
    report true "OK message reports the exemption count (1 listed)"
else
    report false "OK message reports the exemption count; output: $out"
fi

# Case 6 (RED, "make one runner unparseable"): ci_smoke.sh clearly invokes
# shell commands (mentions "python3") but in a format this parser cannot
# recognise at all (no quoted "$SCRIPT_DIR/..." path) -- must refuse, not
# silently treat it as "nothing registered, nothing to check" (that would
# make every real gap invisible, exactly #431's defect).
repo6="$tmpdir/case6"
write_reconciled "$repo6"
cat > "$repo6/tests/ci_smoke.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="$SCRIPT_DIR/test_a.py"
python3 "$TARGET"
EOF
out=$("$GUARD" "$repo6" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "unparseable ci_smoke.sh (indirect variable, no literal path) is refused (exit 1)"
else
    report false "unparseable ci_smoke.sh refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -qi "refusing to approve blind" <<<"$out"; then
    report true "unparseable-ci_smoke failure names the reason (\"refusing to approve blind\")"
else
    report false "unparseable-ci_smoke failure names the reason; output: $out"
fi

# Case 7 (RED, missing runner file): tests/ci_smoke.sh absent entirely --
# cannot reconcile against a runner that does not exist; must refuse.
repo7="$tmpdir/case7"
mkdir -p "$repo7/tests"
cat > "$repo7/tests/run_all.sh" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
run_test() { :; }
run_test "test_a" python3 "$SCRIPT_DIR/test_a.py"
EOF
out=$("$GUARD" "$repo7" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "missing tests/ci_smoke.sh is refused (exit 1)"
else
    report false "missing tests/ci_smoke.sh refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -qi "not found" <<<"$out"; then
    report true "missing-runner failure says so explicitly (\"not found\")"
else
    report false "missing-runner failure says so explicitly; output: $out"
fi

# Case 8 (RED, unparseable/malformed exemption list): an exemption entry
# with no '|' must refuse rather than silently being ignored (an ignored
# malformed entry would mean a reviewer who believes they documented an
# exemption actually did not, and the next run blindly reports a gap the
# wrong way -- or worse, a different parsing of a broken line could
# silently match a real registration name and suppress a genuine gap).
repo8="$tmpdir/case8"
write_reconciled "$repo8"
cat >> "$repo8/tests/ci_smoke.sh" <<'EOF'
run_section "ghost" python3 "$SCRIPT_DIR/test_ghost.py"
EOF
cat > "$repo8/tests/runner_registration_exemptions.list" <<'EOF'
this line has no pipe separator
EOF
out=$("$GUARD" "$repo8" 2>&1); rc=$?
if [[ "$rc" == "1" ]]; then
    report true "malformed exemption-list entry is refused (exit 1)"
else
    report false "malformed exemption-list entry refused (wanted exit 1, got $rc; output: $out)"
fi
if grep -qi "malformed exemption" <<<"$out"; then
    report true "malformed-exemption failure names the reason"
else
    report false "malformed-exemption failure names the reason; output: $out"
fi

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
