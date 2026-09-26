#!/usr/bin/env bash
# test_check_parallel.sh — regression test for scripts/lib/parallel_runner.sh
# (the aggregation core behind `make check`'s concurrent suite runner,
# scripts/check_parallel.sh) against synthetic jobs, so the
# pass/fail-attribution and crash-detection logic is checked on every
# `make check` run without paying for a real node/go/Playwright run.
#
# Usage: tests/test_check_parallel.sh   (no root, no PG, no Playwright)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$REPO_ROOT/scripts/lib/parallel_runner.sh"

echo "=== test_check_parallel ==="

passed=0
failed=0
check() {
    local cond=$1 desc="$2"
    if [[ "$cond" -eq 0 ]]; then
        echo "  PASS: $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL: $desc"
        failed=$((failed + 1))
    fi
}
contains() { [[ "$1" == *"$2"* ]]; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-test-check-parallel.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# ── 1. All jobs pass ─────────────────────────────────────────────────────────
JOB_NAMES=(a b c)
JOB_CMDS=("echo hi-a" "echo hi-b" "echo hi-c")
CAP=3
RESULTS_DIR="$WORK/all_pass"
out=$(run_parallel_jobs); ec=$?
check $([[ $ec -eq 0 ]]; echo $?) "all-pass: run_parallel_jobs returns 0"
contains "$out" "SUITE PASSED: a"; r1=$?
contains "$out" "SUITE PASSED: b"; r2=$?
contains "$out" "SUITE PASSED: c"; r3=$?
check $((r1 || r2 || r3)) "all-pass: every job reported SUITE PASSED"
contains "$out" "PARALLEL CHECK: PASS"; check $? "all-pass: overall verdict is PASS"

# ── 2. One job fails cleanly; the failure is attributed to the RIGHT name ───
JOB_NAMES=(ok bad ok2)
JOB_CMDS=("echo fine" "echo boom; exit 3" "echo also-fine")
CAP=3
RESULTS_DIR="$WORK/one_fail"
out=$(run_parallel_jobs); ec=$?
check $([[ $ec -ne 0 ]]; echo $?) "one-fail: run_parallel_jobs returns non-zero"
contains "$out" "SUITE FAILED: bad (exit 3)"; check $? "one-fail: names the failing suite with its exit code"
contains "$out" "SUITE PASSED: ok"; r1=$?
contains "$out" "SUITE PASSED: ok2"; r2=$?
check $((r1 || r2)) "one-fail: the two passing suites are still reported as passed"
contains "$out" "PARALLEL CHECK: FAIL"; check $? "one-fail: overall verdict is FAIL"

# ── 3. A job whose OWN SHELL is killed (SIGKILL, e.g. OOM) is a CRASH, never
#      a silent pass — this is the case the issue calls out explicitly. ────
JOB_NAMES=(ok killed)
JOB_CMDS=("echo fine" "sleep 30")
CAP=2
RESULTS_DIR="$WORK/crash"
(
    # Launch in the background, SIGKILL the "killed" job's subshell shortly
    # after it starts (while it's blocked in wait() for `sleep 30` — SIGKILL
    # is unconditional even mid-syscall, so it never reaches the "write the
    # .exit sentinel" line), then let run_parallel_jobs finish aggregating.
    run_parallel_jobs > "$WORK/crash_out.txt" 2>&1
    echo $? > "$WORK/crash_ec.txt"
) &
runner_pid=$!
sleep 1
victim=$(pgrep -f "sleep 30" | head -1)
if [[ -n "$victim" ]]; then
    victim_parent=$(ps -o ppid= -p "$victim" | tr -d ' ')
    kill -9 "$victim" 2>/dev/null          # the `sleep 30` child itself
    kill -9 "$victim_parent" 2>/dev/null   # its wrapper subshell (the crash)
fi
wait "$runner_pid" 2>/dev/null
out=$(cat "$WORK/crash_out.txt")
ec=$(cat "$WORK/crash_ec.txt")
check $([[ "$ec" -ne 0 ]]; echo $?) "crash: run_parallel_jobs returns non-zero"
contains "$out" "SUITE CRASHED: killed"; check $? "crash: a killed job's own shell is reported CRASHED, not silently passed"
contains "$out" "SUITE PASSED: ok"; check $? "crash: the unrelated healthy job still passed"

# ── 4. CAP actually serializes: with cap=1, three jobs' start/end markers
#      never interleave (no timing assertion -- pure ordering, so this can't
#      be flaky on a slow/busy runner). ─────────────────────────────────────
tracker="$WORK/tracker.log"
: > "$tracker"
JOB_NAMES=(x y z)
JOB_CMDS=(
    "echo start-x >> '$tracker'; sleep 0.2; echo end-x >> '$tracker'"
    "echo start-y >> '$tracker'; sleep 0.2; echo end-y >> '$tracker'"
    "echo start-z >> '$tracker'; sleep 0.2; echo end-z >> '$tracker'"
)
CAP=1
RESULTS_DIR="$WORK/cap1"
run_parallel_jobs >/dev/null 2>&1
serialized=0
while read -r line1 && read -r line2; do
    name1="${line1#start-}"
    name2="${line2#end-}"
    [[ "$line1" == start-* && "$line2" == end-* && "$name1" == "$name2" ]] || serialized=1
done < "$tracker"
lines=$(wc -l < "$tracker")
check $([[ "$lines" -eq 6 ]]; echo $?) "cap=1: all 3 jobs' start+end markers were written (6 lines)"
check $serialized "cap=1: every job's start is immediately followed by its OWN end (no interleaving)"

echo
echo "$passed/$((passed + failed)) passed"
exit $((failed > 0 ? 1 : 0))
