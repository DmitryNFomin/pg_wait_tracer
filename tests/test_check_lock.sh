#!/usr/bin/env bash
# test_check_lock.sh -- bypass suite for scripts/check-lock.sh (+
# scripts/check-lock.py), the machine-wide mutual exclusion around
# `make check` (issue: raising the in-flight-agent limit to three on one
# Mac). Per the implementer contract's bypass-suite clause: a test per way
# this mechanism can fail open or fail wrong.
#
# History: a first version judged staleness in userspace (recorded pid +
# `ps` start-time fingerprint) and reclaimed a dead holder's lock by
# `mv`-ing a challenger's claim over it, then reading the result back. A
# concurrency review reproduced double-holds (case "N" below, run against
# that version, red 3/20 -- see git log for the exact numbers) because
# rename-then-read-back is two syscalls, not one atomic operation. The
# fix replaces that with a real kernel lock (flock(2) via Python's
# `fcntl.flock`, see scripts/check-lock.py's docstring) -- there is no
# userspace staleness judgment left to get wrong, so the old "reused pid"
# and "kill(pid,0) permission error" failure modes are gone by
# construction, not patched over.
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

# race_once LOCKFILE LOGFILE N_WAITERS -- launches N_WAITERS concurrent
# check-lock.sh runs against LOCKFILE, each appending a start/end pair
# (with nanosecond timestamps) to LOGFILE, then reports via stdout
# "OK" or "OVERLAP" (whether any two critical sections' [start,end]
# intervals overlapped).
race_once() {
    local lockfile="$1" logfile="$2" n="$3"
    : > "$logfile"
    local pids=()
    local w
    for ((w = 1; w <= n; w++)); do
        (cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$lockfile" PGWT_CHECK_LOCK_POLL=1 "$LOCK_BIN" \
            bash -c "echo start:$w:\$(date +%s%N) >> '$logfile'; sleep 0.15; echo end:$w:\$(date +%s%N) >> '$logfile'") \
            > /dev/null 2>&1 &
        pids+=($!)
    done
    wait "${pids[@]}" 2>/dev/null
    python3 - "$logfile" <<'PYEOF'
import sys
events = []
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    kind, w, ts = line.split(":")
    events.append((int(ts), 0 if kind == "end" else 1, kind, w))
# ends sort before starts at the same timestamp so a zero-width gap is
# never mistaken for an overlap.
events.sort()
active = set()
overlap = False
for _, _, kind, w in events:
    if kind == "start":
        if active:
            overlap = True
        active.add(w)
    else:
        active.discard(w)
print("OVERLAP" if overlap else "OK")
PYEOF
}

# ---------------------------------------------------------------------------
# 0. Baseline: uncontended acquire runs the command and propagates its
#    exit code.
# ---------------------------------------------------------------------------
lf="$tmpdir/case0.lock"
out=$(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" echo "hello"); rc=$?
[[ "$out" == "hello" && $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "uncontended acquire runs the wrapped command and returns its exit code"

(cd "$REPO_ROOT" && PGWT_CHECK_LOCK_FILE="$tmpdir/case0b.lock" "$LOCK_BIN" bash -c 'exit 7')
rc=$?
[[ $rc -eq 7 ]] && ok=true || ok=false
report "$ok" "wrapped command's own exit code is propagated (got $rc, want 7)"

# ---------------------------------------------------------------------------
# 1. BYPASS: two starts racing for the lock at the same instant, against a
#    lock file that does not exist yet (the "nobody has ever held this"
#    path). Must still be fully serialized.
# ---------------------------------------------------------------------------
result=$(race_once "$tmpdir/case1.lock" "$tmpdir/case1.race.log" 2)
[[ "$result" == "OK" ]] && ok=true || ok=false
report "$ok" "two fresh-file racing starts are fully serialized (result: $result)"

# ---------------------------------------------------------------------------
# 2. BYPASS: a crash (SIGKILL, e.g. this machine's memory manager) must not
#    wedge the lock forever. Kill the holder mid-check; a waiter must
#    acquire cleanly afterwards, not hang.
# ---------------------------------------------------------------------------
lf="$tmpdir/case2.lock"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" sleep 60 &
holder=$!
# There is no lock FILE-existence signal anymore (the file is created
# empty up front and flock'd in place) -- wait for the holder's pid to
# actually be running instead.
for _ in $(seq 1 50); do kill -0 "$holder" 2>/dev/null && break; sleep 0.1; done
kill -0 "$holder" 2>/dev/null && ok=true || ok=false
report "$ok" "holder process is running before being killed"
sleep 0.3  # give it a moment to actually reach the flock'd section
kill -9 "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
waiter_out=$(timeout 15 env PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 \
    "$LOCK_BIN" echo "acquired after SIGKILL")
waiter_rc=$?
[[ $waiter_rc -eq 0 && "$waiter_out" == "acquired after SIGKILL" ]] && ok=true || ok=false
report "$ok" "a waiter acquires cleanly after the holder is SIGKILLed (rc=$waiter_rc out='$waiter_out')"

# ---------------------------------------------------------------------------
# 3. BYPASS (the reviewer-reproduced blocker): SEVERAL concurrent waiters
#    against ONE fabricated lock file with garbage/stale-looking content
#    (an old design's "dead holder" record) but -- critically -- no
#    process actually holding the OS-level flock on it. This is exactly
#    the shape of the double-hold bug: repeat across many iterations and
#    assert NO iteration ever has two overlapping critical sections.
#    Against the fixed (flock-based) check-lock.py this must be 0/N.
# ---------------------------------------------------------------------------
n_waiters=8
n_iters=15
overlaps=0
for ((iter = 1; iter <= n_iters; iter++)); do
    lf="$tmpdir/case3.iter$iter.lock"
    # Garbage content mimicking an old-format "dead holder" record. It
    # must be completely irrelevant to correctness now: nobody has an
    # actual flock on this fd, so every waiter is equally free to take it.
    cat > "$lf" <<'EOF'
pid=999999
fingerprint=Wed Jan  1 00:00:00 2020
host=nowhere
user=nobody
label=fabricated-stale-holder (garbage content, no live flock)
cmd=fake
acquired_at=1
EOF
    result=$(race_once "$lf" "$tmpdir/case3.iter$iter.race.log" "$n_waiters")
    [[ "$result" == "OVERLAP" ]] && overlaps=$((overlaps + 1))
done
[[ $overlaps -eq 0 ]] && ok=true || ok=false
report "$ok" "$n_waiters concurrent waiters x $n_iters iterations against a fabricated stale lock: $overlaps/$n_iters had an overlapping critical section (want 0)"

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
#    foreground process group) must leave no debris, must never have run
#    its wrapped command, and a later fresh acquire must still work.
# ---------------------------------------------------------------------------
lf="$tmpdir/case5.lock"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" sleep 6 > /dev/null 2>&1 &
holder=$!
for _ in $(seq 1 50); do kill -0 "$holder" 2>/dev/null && break; sleep 0.1; done
sleep 0.3
waiter_marker="$tmpdir/case5.waiter.ran"
rm -f "$waiter_marker"
PGWT_CHECK_LOCK_FILE="$lf" PGWT_CHECK_LOCK_POLL=1 PGWT_CHECK_LOCK_NOTIFY=1 \
    "$LOCK_BIN" touch "$waiter_marker" > /dev/null 2>&1 &
waiter=$!
sleep 1
kill -INT -- -"$waiter" 2>/dev/null
wait "$waiter" 2>/dev/null
waiter_rc=$?
[[ ! -e "$waiter_marker" ]] && ok=true || ok=false
report "$ok" "the interrupted waiter never ran its wrapped command (rc=$waiter_rc)"
[[ $waiter_rc -ge 128 ]] && ok=true || ok=false
report "$ok" "interrupted waiter's exit code reflects the signal (rc=$waiter_rc, want >=128)"
sidecars=$(ls "${lf}".* 2>/dev/null | wc -l | tr -d ' ')
[[ "$sidecars" -eq 0 ]] && ok=true || ok=false
report "$ok" "interrupted waiter leaves no sidecar/claim debris next to the lock file (found $sidecars)"
wait "$holder" 2>/dev/null
holder_rc=$?
[[ $holder_rc -eq 0 ]] && ok=true || ok=false
report "$ok" "the real holder still completed and released cleanly despite the unrelated interrupted waiter (rc=$holder_rc)"
fresh_out=$(timeout 10 env PGWT_CHECK_LOCK_FILE="$lf" "$LOCK_BIN" echo "fresh acquire ok")
fresh_rc=$?
[[ $fresh_rc -eq 0 && "$fresh_out" == "fresh acquire ok" ]] && ok=true || ok=false
report "$ok" "a fresh acquire after the interrupted-waiter episode still works (rc=$fresh_rc out='$fresh_out')"

# ---------------------------------------------------------------------------
# 6. BYPASS: the lock tool itself (python3, since macOS has no flock(1) and
#    the real locking primitive is flock(2) via `fcntl.flock`) is missing
#    from PATH. Must fail loudly, never silently run the wrapped command
#    unlocked.
# ---------------------------------------------------------------------------
minpath="$tmpdir/minpath"
mkdir -p "$minpath"
for t in bash sed cat; do
    p="$(command -v "$t" 2>/dev/null)" && ln -s "$p" "$minpath/$t"
done
# Deliberately omit python3 from minpath.
marker6="$tmpdir/case6.marker"
rm -f "$marker6"
err6=$(PATH="$minpath" bash "$LOCK_BIN" touch "$marker6" 2>&1)
rc6=$?
[[ $rc6 -ne 0 ]] && ok=true || ok=false
report "$ok" "missing lock tool (python3): check-lock.sh exits non-zero (rc=$rc6)"
[[ ! -e "$marker6" ]] && ok=true || ok=false
report "$ok" "missing lock tool (python3): the wrapped command never ran (fail closed, not silently unlocked)"
[[ "$err6" == *"missing required tool"* ]] && ok=true || ok=false
report "$ok" "missing lock tool (python3): error names the missing tool (output: $err6)"

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
