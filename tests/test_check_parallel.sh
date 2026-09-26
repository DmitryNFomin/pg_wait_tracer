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

# ── 5. The live-log hint is printed before jobs run (review: with all output
#      buffered until the end, a hang looks identical to normal progress —
#      say where to look while it's still running). ─────────────────────────
JOB_NAMES=(a b)
JOB_CMDS=("echo hi-a" "echo hi-b")
CAP=2
RESULTS_DIR="$WORK/hint"
out=$(run_parallel_jobs)
contains "$out" "tail -f $WORK/hint"; check $? "hint: prints the results dir with a tail -f follow hint"

# ── 6-10. Bypass suite (spirit of #183): a gate that cannot see its own job
#      list must refuse, never approve. Each of these is a way a suite could
#      "never start" yet never be reported as failed either (invisible, not
#      even CRASHED) — or deadlock forever. Every case must exit non-zero,
#      print nothing claiming PASS, and (where applicable) create no results. ─

# 6. Empty job list.
JOB_NAMES=()
JOB_CMDS=()
CAP=3
RESULTS_DIR="$WORK/empty"
out=$(run_parallel_jobs 2>&1); ec=$?
check $([[ $ec -ne 0 ]]; echo $?) "bypass: empty JOB_NAMES refuses (non-zero)"
contains "$out" "PASS"; check $([[ $? -ne 0 ]]; echo $?) "bypass: empty JOB_NAMES never claims a PASS"

# 7. JOB_NAMES/JOB_CMDS length mismatch — a dropped command would otherwise
#    just be skipped by the `for i in "${!JOB_NAMES[@]}"` loop: it never runs
#    and is never in the report, so the run could still say PASS.
JOB_NAMES=(a b)
JOB_CMDS=("echo hi-a" "echo hi-b" "echo hi-c")
CAP=3
RESULTS_DIR="$WORK/mismatch"
out=$(run_parallel_jobs 2>&1); ec=$?
check $([[ $ec -ne 0 ]]; echo $?) "bypass: JOB_NAMES/JOB_CMDS length mismatch refuses"
contains "$out" "PARALLEL CHECK: PASS"; check $([[ $? -ne 0 ]]; echo $?) "bypass: mismatch never claims PARALLEL CHECK: PASS"
[[ -d "$WORK/mismatch" ]]; check $([[ $? -ne 0 ]]; echo $?) "bypass: mismatch never even creates RESULTS_DIR (refused up front)"

# 8. An empty command string: `bash -c ""` exits 0, which would otherwise be
#    reported as SUITE PASSED without checking anything.
JOB_NAMES=(a empty_cmd)
JOB_CMDS=("echo hi-a" "")
CAP=3
RESULTS_DIR="$WORK/emptycmd"
out=$(run_parallel_jobs 2>&1); ec=$?
check $([[ $ec -ne 0 ]]; echo $?) "bypass: an empty command string refuses"
contains "$out" "SUITE PASSED: empty_cmd"; check $([[ $? -ne 0 ]]; echo $?) "bypass: the empty-command job is never reported PASSED"

# 9. CAP=0 would deadlock (the semaphore starts with zero slots, so the very
#    first `read -u 8` blocks forever) — refuse instead of hanging `make
#    check`. Run in a fresh `bash -c` under `timeout` so a regression that
#    reintroduces the deadlock fails this test instead of hanging it forever.
out=$(timeout 5 bash -c "source '$REPO_ROOT/scripts/lib/parallel_runner.sh'; JOB_NAMES=(a); JOB_CMDS=('echo hi-a'); CAP=0; RESULTS_DIR='$WORK/cap0'; run_parallel_jobs" 2>&1); ec=$?
check $([[ $ec -eq 1 ]]; echo $?) "bypass: CAP=0 refuses immediately (not: times out deadlocked)"

# 10. A non-integer CAP (e.g. an unset/mistyped env var) — same refusal path.
JOB_NAMES=(a)
JOB_CMDS=("echo hi-a")
CAP="not-a-number"
RESULTS_DIR="$WORK/capnan"
out=$(run_parallel_jobs 2>&1); ec=$?
check $([[ $ec -ne 0 ]]; echo $?) "bypass: a non-integer CAP refuses"

# 11. mkfifo failing (e.g. a TMPDIR that doesn't support FIFOs) must refuse
#     loudly, not fall through to `exec 8<>` silently opening a plain file —
#     which would make the semaphore stop blocking (every job launches at
#     once) with no diagnostic at all. Shadow the external `mkfifo` command
#     with a function of the same name: bash resolves that before PATH.
JOB_NAMES=(a)
JOB_CMDS=("echo hi-a")
CAP=1
RESULTS_DIR="$WORK/mkfifofail"
mkfifo() { return 1; }
out=$(run_parallel_jobs 2>&1); ec=$?
unset -f mkfifo
check $([[ $ec -ne 0 ]]; echo $?) "bypass: a failing mkfifo refuses instead of degrading silently"
contains "$out" "mkfifo failed"; check $? "bypass: the mkfifo failure names itself in the refusal message"

# ── 12. Two sequential calls with DIFFERENT RESULTS_DIR values (the pattern
#      scripts/check_parallel.sh now uses: a Phase 1 pool, then a solo Phase 2
#      for the one contention-sensitive suite) must not let the second call's
#      `rm -rf "$RESULTS_DIR"` clobber the first call's logs. ───────────────
JOB_NAMES=(p1a p1b)
JOB_CMDS=("echo phase1-a" "echo phase1-b")
CAP=2
RESULTS_DIR="$WORK/phase1"
run_parallel_jobs >/dev/null 2>&1

JOB_NAMES=(p2)
JOB_CMDS=("echo phase2")
CAP=1
RESULTS_DIR="$WORK/phase2"
run_parallel_jobs >/dev/null 2>&1

[[ -f "$WORK/phase1/p1a.log" && -f "$WORK/phase1/p1b.log" ]]
check $? "two-phase: phase 1's logs survive a later call with a different RESULTS_DIR"
[[ -f "$WORK/phase2/p2.log" ]]
check $? "two-phase: phase 2's own log exists too"

echo
echo "$passed/$((passed + failed)) passed"
exit $((failed > 0 ? 1 : 0))
