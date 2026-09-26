#!/usr/bin/env bash
# test_check_lock.sh -- bypass suite for scripts/check-lock.sh, the
# machine-wide mutual exclusion around `make check` (issue: raising the
# in-flight-agent limit to three on one Mac). Per the implementer contract's
# bypass-suite clause: a test per way this mechanism can fail open or fail
# wrong.
#
# Every case below runs against a throwaway lock file under `mktemp -d`
# (removed on exit) via PGWT_CHECK_LOCK_FILE -- never the real
# /tmp/pgwt-check.lock -- so this never collides with a real concurrent
# `make check` on the same machine.
#
# Uses `set -m` (job control) so background jobs get their own process
# group: a SIGINT sent with `kill -- -PID` reaches both check-lock.sh AND
# whatever foreground child it is currently running/waiting on, exactly
# like a terminal's Ctrl-C would -- `kill -INT PID` alone does not (bash
# defers a plain single-pid SIGINT until the foreground child exits).
#
# Wired into `make check` via the Makefile's `check:` recipe (NOT
# scripts/check.sh -- a concurrent branch owns that file's internals right
# now; this suite plus the lock wrapper are additive around it).
#
# Usage: tests/test_check_lock.sh (run from anywhere)
set -um
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCK_BIN="$SCRIPT_DIR/../scripts/check-lock.sh"
REPO_ROOT="$SCRIPT_DIR/.."

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

run_locked() {
    # run_locked LOCKFILE cmd... -- convenience wrapper, cd'd to repo root
    # so a relative-path invocation from any cwd still finds check-lock.sh.
    (cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$1" "${@:2}")
}

# ---------------------------------------------------------------------------
# 0. Baseline: uncontended acquire runs the command, propagates its exit
#    code, and leaves no lock file behind afterwards.
# ---------------------------------------------------------------------------
lf="$tmpdir/case0.lock"
out=$(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" echo "hello"); rc=$?
[[ "$out" == "hello" && $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "uncontended acquire runs the wrapped command and returns its exit code"
[[ ! -e "$lf" ]] && ok=true || ok=false
report "$ok" "lock file is removed after a normal run"

(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$tmpdir/case0b.lock" "$LOCK_BIN" bash -c 'exit 7')
rc=$?
[[ $rc -eq 7 ]] && ok=true || ok=false
report "$ok" "wrapped command's own exit code is propagated (got $rc, want 7)"

# ---------------------------------------------------------------------------
# 1. BYPASS: two starts racing for the lock at the same instant. A lock
#    that occasionally lets both through is worse than none. Two processes
#    launched back-to-back with no stagger must still be fully serialized:
#    the second one's "start" marker must never appear before the first
#    one's "end" marker.
# ---------------------------------------------------------------------------
lf="$tmpdir/case1.lock"
racelog="$tmpdir/case1.race.log"
: > "$racelog"
export racelog
(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" \
    bash -c '{ echo "start:A"; sleep 1; echo "end:A"; } >> "$racelog"') &
r1=$!
(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" \
    bash -c '{ echo "start:B"; sleep 1; echo "end:B"; } >> "$racelog"') &
r2=$!
wait "$r1" "$r2"
mapfile -t lines < "$racelog"
if [[ "${#lines[@]}" -eq 4 ]] \
    && [[ "${lines[0]}" == start:* ]] \
    && [[ "${lines[1]}" == "end:${lines[0]#start:}" ]] \
    && [[ "${lines[2]}" == start:* ]] \
    && [[ "${lines[2]}" != "${lines[0]}" ]] \
    && [[ "${lines[3]}" == "end:${lines[2]#start:}" ]]; then
    ok=true
else
    ok=false
fi
report "$ok" "two racing starts are fully serialized, never interleaved (log: ${lines[*]:-<empty>})"

# ---------------------------------------------------------------------------
# 2. BYPASS: a crash (SIGKILL, e.g. this machine's memory manager) must not
#    wedge the lock forever. Kill the holder mid-check; a waiter must
#    acquire cleanly afterwards, not hang.
# ---------------------------------------------------------------------------
lf="$tmpdir/case2.lock"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" sleep 60 &
holder=$!
for _ in $(seq 1 50); do [[ -f "$lf" ]] && break; sleep 0.1; done
[[ -f "$lf" ]] && ok=true || ok=false
report "$ok" "holder acquired the lock before being killed"
kill -9 "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
waiter_out=$(timeout 15 env PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 \
    "$LOCK_BIN" echo "acquired after SIGKILL")
waiter_rc=$?
[[ $waiter_rc -eq 0 && "$waiter_out" == "acquired after SIGKILL" ]] && ok=true || ok=false
report "$ok" "a waiter acquires cleanly after the holder is SIGKILLed (rc=$waiter_rc out='$waiter_out')"

# ---------------------------------------------------------------------------
# 3. BYPASS: the holder's pid gets reused by an unrelated (still live!)
#    process. kill(pid, 0) alone -- what shlock itself uses, and its own
#    man page lists as a bug -- would see "alive" and wedge forever.
#    Staleness must be judged by pid identity (a start-time fingerprint),
#    not just liveness.
# ---------------------------------------------------------------------------
lf="$tmpdir/case3.lock"
cat > "$lf" <<EOF
pid=$$
fingerprint=Wed Jan  1 00:00:00 2020
host=nowhere
user=nobody
label=fabricated-stale-holder (simulates a reused pid)
cmd=fake
acquired_at=1
nonce=fake-nonce
EOF
# $$ (this test script) is unquestionably alive, so a naive "kill -0 only"
# check would treat this lock as held forever.
out=$(timeout 15 env PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 \
    "$LOCK_BIN" echo "reclaimed despite live pid")
rc=$?
[[ $rc -eq 0 && "$out" == "reclaimed despite live pid" ]] && ok=true || ok=false
report "$ok" "a live pid with a mismatched fingerprint (reused pid) is treated as stale, not wedged (rc=$rc out='$out')"

# ---------------------------------------------------------------------------
# 4. BYPASS: the lock directory is unwritable. Must fail LOUDLY and never
#    run the wrapped command unlocked (fail closed, not fail open).
# ---------------------------------------------------------------------------
noperm="$tmpdir/noperm"
mkdir -p "$noperm"
chmod 555 "$noperm"
marker="$tmpdir/case4.marker"
rm -f "$marker"
err=$( (cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$noperm/check.lock" "$LOCK_BIN" touch "$marker") 2>&1 )
rc=$?
[[ $rc -ne 0 ]] && ok=true || ok=false
report "$ok" "unwritable lock directory: check-lock.sh exits non-zero (rc=$rc)"
[[ ! -e "$marker" ]] && ok=true || ok=false
report "$ok" "unwritable lock directory: the wrapped command never ran (fail closed)"
[[ "$err" == *"unlocked"* ]] && ok=true || ok=false
report "$ok" "unwritable lock directory: error says it is refusing to run unlocked (output: $err)"
chmod 755 "$noperm"

# ---------------------------------------------------------------------------
# 5. BYPASS: an interrupted waiter (Ctrl-C, i.e. SIGINT to the whole
#    foreground process group) must leave no debris -- and the real
#    holder's lock must be untouched, and a later fresh acquire must still
#    work.
# ---------------------------------------------------------------------------
lf="$tmpdir/case5.lock"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" sleep 6 > /dev/null 2>&1 &
holder=$!
for _ in $(seq 1 50); do [[ -f "$lf" ]] && break; sleep 0.1; done
waiter_marker="$tmpdir/case5.waiter.ran"
rm -f "$waiter_marker"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" touch "$waiter_marker" > /dev/null 2>&1 &
waiter=$!
for _ in $(seq 1 50); do
    ls "${lf}.claim."* >/dev/null 2>&1 && break
    sleep 0.1
done
claim_before=$(ls "${lf}".claim.* 2>/dev/null | wc -l | tr -d ' ')
[[ "$claim_before" -ge 1 ]] && ok=true || ok=false
report "$ok" "waiter created its claim file while waiting (found $claim_before)"
kill -INT -- -"$waiter" 2>/dev/null
wait "$waiter" 2>/dev/null
waiter_rc=$?
claim_after=$(ls "${lf}".claim.* 2>/dev/null | wc -l | tr -d ' ')
[[ "$claim_after" -eq 0 ]] && ok=true || ok=false
report "$ok" "interrupted waiter leaves no claim-file debris (found $claim_after after SIGINT, rc=$waiter_rc)"
[[ -f "$lf" ]] && ok=true || ok=false
report "$ok" "the real holder's lock file is untouched by the interrupted waiter"
[[ ! -e "$waiter_marker" ]] && ok=true || ok=false
report "$ok" "the interrupted waiter never ran its wrapped command"
wait "$holder" 2>/dev/null
[[ ! -e "$lf" ]] && ok=true || ok=false
report "$ok" "the real holder still released cleanly after the unrelated waiter was interrupted"
fresh_out=$(timeout 10 env PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" echo "fresh acquire ok")
fresh_rc=$?
[[ $fresh_rc -eq 0 && "$fresh_out" == "fresh acquire ok" ]] && ok=true || ok=false
report "$ok" "a fresh acquire after the interrupted-waiter episode still works (rc=$fresh_rc out='$fresh_out')"

# ---------------------------------------------------------------------------
# 6. BYPASS: the lock tool itself (here: `ln`, the primitive this locking
#    scheme is built on) is missing from PATH on this platform. Must fail
#    loudly, never silently run the wrapped command unlocked.
# ---------------------------------------------------------------------------
minpath="$tmpdir/minpath"
mkdir -p "$minpath"
for t in bash sed rm cat date mv kill ps hostname id; do
    p="$(command -v "$t" 2>/dev/null)" && ln -s "$p" "$minpath/$t"
done
# Deliberately omit `ln` from minpath.
marker6="$tmpdir/case6.marker"
rm -f "$marker6"
err6=$(PATH="$minpath" PGWT_CHECK_LOCK_FILE="$tmpdir/case6.lock" bash "$LOCK_BIN" touch "$marker6" 2>&1)
rc6=$?
[[ $rc6 -ne 0 ]] && ok=true || ok=false
report "$ok" "missing lock tool (ln): check-lock.sh exits non-zero (rc=$rc6)"
[[ ! -e "$marker6" ]] && ok=true || ok=false
report "$ok" "missing lock tool (ln): the wrapped command never ran (fail closed, not silently unlocked)"
[[ "$err6" == *"missing required tool"* ]] && ok=true || ok=false
report "$ok" "missing lock tool (ln): error names the missing tool (output: $err6)"

# ---------------------------------------------------------------------------
# 7. Human escape hatch: PGWT_SKIP_CHECK_LOCK=1 bypasses locking entirely
#    (documented, mirrors PGWT_SKIP_PUSH_GUARD).
# ---------------------------------------------------------------------------
out7=$(cd "$REPO_ROOT" && PGWT_SKIP_CHECK_LOCK=1 PGWT_CHECK_LOCK_FILE="$noperm/would-fail.lock" "$LOCK_BIN" echo "bypassed")
rc7=$?
[[ $rc7 -eq 0 && "$out7" == "bypassed" ]] && ok=true || ok=false
report "$ok" "PGWT_SKIP_CHECK_LOCK=1 bypasses locking even when the lock dir is unwritable (escape hatch works)"

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
