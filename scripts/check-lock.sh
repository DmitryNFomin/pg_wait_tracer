#!/usr/bin/env bash
# check-lock.sh -- machine-wide mutual exclusion around `make check`.
#
# Why: raising the in-flight-agent limit means several agent worktrees on
# this ONE Mac can try to run `make check` at the same time. `make check`
# runs seven-ish Playwright/chaos suites; concurrent runs compete for CPU
# and produce noise-driven flakes (measured: ~1-in-3 on a tight assertion),
# and this machine's memory manager has SIGKILLed long jobs outright. A red
# that means nothing teaches people to re-run instead of read, so instead of
# letting checks collide, they queue: this script serialises them.
#
# Platform note: this only ever runs on macOS (nothing eBPF-related does,
# and `make check` is the Mac-only tier). macOS has no `flock(1)`; `shlock`
# exists but is explicitly deprecated AND documented to mishandle PID reuse
# (see `man shlock` BUGS). So this implements its own tiny lock using two
# POSIX primitives that ARE atomic everywhere, including macOS:
#   - link(2) (the `ln` command): create-if-absent, atomically -- exactly
#     the same primitive shlock itself is built on.
#   - rename(2) (the `mv` command): atomically replace one regular file
#     with another -- used only to reclaim a lock whose holder is
#     confirmed dead, with a read-back-and-compare-a-nonce step so that
#     if two waiters both try to reclaim at once, exactly one of them
#     ends up believing it holds the lock (whichever's rename() the
#     filesystem orders last -- the loser sees the winner's nonce on
#     read-back and goes back to waiting).
#
# Stale-lock detection is NOT just "is this pid alive": a dead holder's pid
# can be reused by an unrelated live process (see man shlock BUGS -- shlock
# gets this wrong). The lock file also records the holder process's start
# time (`ps -o lstart=`); a lock is only "alive" if the recorded pid is
# alive AND its start time still matches. A pid that's alive but with a
# different start time is a reused pid, i.e. a dead holder -- reclaimed the
# same as an outright-dead pid.
#
# Escape hatch for a human (mirrors PGWT_SKIP_PUSH_GUARD): PGWT_SKIP_CHECK_LOCK=1
# runs the wrapped command with no locking at all. Not for agents.
#
# Usage: scripts/check-lock.sh <command> [args...]
set -uo pipefail

if [[ "${PGWT_SKIP_CHECK_LOCK:-}" == "1" ]]; then
    exec "$@"
fi

LOCK_FILE="${PGWT_CHECK_LOCK_FILE:-/tmp/pgwt-check.lock}"
POLL="${PGWT_CHECK_LOCK_POLL:-5}"
NOTIFY_EVERY="${PGWT_CHECK_LOCK_NOTIFY:-20}"
cmd_desc="$*"

# A lock that silently fails to lock is worse than no lock at all -- so a
# missing tool this depends on is a loud, immediate, fail-closed error, not
# a silent fall-through to running unlocked.
need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "check-lock: missing required tool '$1' -- refusing to run" \
             "'$cmd_desc' unlocked. Fix PATH, or (humans only) set" \
             "PGWT_SKIP_CHECK_LOCK=1 to bypass this lock deliberately." >&2
        exit 2
    }
}
need ln; need mv; need kill; need ps; need date; need sed; need rm

claim_tmp="${LOCK_FILE}.claim.$$"
we_hold=0

cleanup() {
    rm -f "$claim_tmp" 2>/dev/null
    if [[ "$we_hold" == "1" ]]; then
        rm -f "$LOCK_FILE" 2>/dev/null
    fi
}
trap cleanup EXIT

fingerprint_of() {
    # A "who is this pid, really" fingerprint that changes if the pid
    # number gets reused by a different process later: the process's own
    # start time. Whitespace-collapsed so string comparison is exact.
    ps -o lstart= -p "$1" 2>/dev/null | tr -s ' '
}

self_fingerprint="$(fingerprint_of $$)"
if [[ -z "$self_fingerprint" ]]; then
    echo "check-lock: could not read our own process fingerprint (ps -o lstart= -p $$) -- refusing to run unlocked." >&2
    exit 2
fi

nonce="$$-$RANDOM-$(date +%s)"
label="$(pwd) ($(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'no-git'))"

write_claim() {
    {
        echo "pid=$$"
        echo "fingerprint=$self_fingerprint"
        echo "host=$(hostname 2>/dev/null || echo unknown-host)"
        echo "user=$(id -un 2>/dev/null || echo unknown-user)"
        echo "label=$label"
        echo "cmd=$*"
        echo "acquired_at=$(date +%s)"
        echo "nonce=$nonce"
    } > "$claim_tmp"
}

field() { sed -n "s/^$2=//p" "$1" | head -n1; }

# Returns 0 iff $LOCK_FILE's recorded holder is still that exact live
# process (not just "some live process now has that pid").
holder_alive() {
    local pid fp curfp
    [[ -f "$LOCK_FILE" ]] || return 1
    pid="$(field "$LOCK_FILE" pid)"
    fp="$(field "$LOCK_FILE" fingerprint)"
    [[ -n "$pid" && -n "$fp" ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    curfp="$(fingerprint_of "$pid")"
    [[ -n "$curfp" && "$curfp" == "$fp" ]]
}

print_status() {
    local pid host user lbl acquired_at now elapsed
    pid="$(field "$LOCK_FILE" pid)"
    host="$(field "$LOCK_FILE" host)"
    user="$(field "$LOCK_FILE" user)"
    lbl="$(field "$LOCK_FILE" label)"
    acquired_at="$(field "$LOCK_FILE" acquired_at)"
    now="$(date +%s)"
    elapsed=$(( now - ${acquired_at:-now} ))
    echo "check-lock: waiting for 'make check' -- this is expected with" \
         "several agents on one Mac, not a hang. Held by $lbl" \
         "(pid $pid, $user@$host) for ${elapsed}s; re-checking every ${POLL}s..." >&2
}

# --- write our claim once up front; a permission problem here (unwritable
# lock directory) must abort loudly, never fall through to "run unlocked". ---
if ! write_claim "$@"; then
    echo "check-lock: cannot write '$claim_tmp' -- lock directory unwritable?" \
         "Refusing to run '$*' unlocked. (humans only: PGWT_SKIP_CHECK_LOCK=1)" >&2
    exit 2
fi

last_print=0
while :; do
    if ln "$claim_tmp" "$LOCK_FILE" 2>/dev/null; then
        we_hold=1
        rm -f "$claim_tmp" 2>/dev/null
        break
    fi

    if [[ ! -e "$LOCK_FILE" ]]; then
        # ln failed but the target still doesn't exist: not a "someone
        # else holds it" EEXIST, but a real error (e.g. the lock
        # directory itself is unwritable/gone). Fail loudly, not silently.
        echo "check-lock: could not create '$LOCK_FILE' (not because it" \
             "exists) -- refusing to run '$*' unlocked." >&2
        exit 2
    fi

    if holder_alive; then
        now="$(date +%s)"
        if (( now - last_print >= NOTIFY_EVERY )); then
            print_status
            last_print=$now
        fi
        sleep "$POLL"
        continue
    fi

    # The recorded holder is dead (or its pid was reused) -- reclaim.
    # rename() is atomic and total-ordered: if several waiters race this
    # at once, exactly one of their nonces is the one left on disk.
    if ! mv -f "$claim_tmp" "$LOCK_FILE" 2>/dev/null; then
        echo "check-lock: failed to reclaim stale lock '$LOCK_FILE'" \
             "-- refusing to run '$*' unlocked." >&2
        exit 2
    fi
    if [[ "$(field "$LOCK_FILE" nonce)" == "$nonce" ]]; then
        we_hold=1
        break
    fi
    # Lost the reclaim race to another waiter; claim_tmp was consumed by
    # our own mv above, so re-materialise it before looping back.
    write_claim "$@"
    last_print=0
done

"$@"
rc=$?
exit "$rc"
