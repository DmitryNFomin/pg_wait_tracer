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
# The actual locking primitive lives in scripts/check-lock.py: macOS has no
# `flock(1)`, so it uses Python's `fcntl.flock` (a real flock(2) kernel
# lock) -- `make check` already hard-requires python3, so this adds no new
# dependency. See that file's docstring for why a real kernel lock replaced
# an earlier userspace-staleness design that a concurrency review found
# could double-acquire.
#
# Escape hatch for a human (mirrors PGWT_SKIP_PUSH_GUARD): PGWT_SKIP_CHECK_LOCK=1
# runs the wrapped command with no locking at all. Not for agents.
#
# Usage: scripts/check-lock.sh <command> [args...]
set -uo pipefail

if [[ "${PGWT_SKIP_CHECK_LOCK:-}" == "1" ]]; then
    exec "$@"
fi

# A lock that silently fails to lock is worse than no lock at all -- so a
# missing tool this depends on is a loud, immediate, fail-closed error, not
# a silent fall-through to running unlocked.
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-lock: missing required tool 'python3' -- refusing to run" \
         "'$*' unlocked. Fix PATH, or (humans only) set" \
         "PGWT_SKIP_CHECK_LOCK=1 to bypass this lock deliberately." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/check-lock.py" "$@"
