#!/usr/bin/env python3
"""agent-ledger.py -- a small flock-guarded ledger of agent_ids seen between
SubagentStart and SubagentStop. sweep.sh counts it as "agents currently
running" (see sweep.sh section [9] and sweep-hook.sh's escalation gate).

Why a ledger instead of inferring from `git worktree list`: a worktree is
evidence an agent ONCE existed -- CLAUDE.md deliberately retains it until its
PR opens -- not that one is running now. On this repo that proxy read 227
live `agent/*` worktrees against a cap of 3, which made `running >= cap`
permanently true and permanently paused the dispatcher's escalation gate: a
gate that can never fire is the mirror image of a gate that can never fail.
This ledger counts actual SubagentStart/SubagentStop pairs instead, which is
the thing the cap (CLAUDE.md: "three tasks actively implementing") is
actually about, at the cost of also counting non-implementer subagents
(reviewers, advisers, Explore) that fire the same hooks -- a real but much
smaller overcount than two orders of magnitude.

Known gap: an agent killed hard (not a clean SubagentStop) leaves an orphan
entry. Bounded two ways: `count` only counts entries younger than MAX_AGE_S,
and sweep.sh independently refuses the whole running count as UNKNOWN if it
is still implausible for a 3-agent-cap project after that filter (its own
SANITY_MAX) -- a proxy that comes back absurd is refused, not trusted.

Locking: a real kernel flock (fcntl.flock), same primitive and same reason as
scripts/check-lock.py (macOS has no flock(1); python3 is already a hard
dependency here, including in this very hook chain).
"""
import fcntl
import os
import sys
import time

MAX_AGE_S = 6 * 3600  # 6h: generous above this project's observed task length


def _with_lock(path, fn):
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        return fn(fd)
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


def _read_lines(fd):
    os.lseek(fd, 0, os.SEEK_SET)
    data = os.read(fd, 1 << 20).decode("utf-8", "replace")
    return [l for l in data.splitlines() if l.strip()]


def _write_lines(fd, lines):
    os.lseek(fd, 0, os.SEEK_SET)
    os.ftruncate(fd, 0)
    os.write(fd, ("\n".join(lines) + ("\n" if lines else "")).encode())


def cmd_start(path, agent_id):
    def fn(fd):
        lines = _read_lines(fd)
        if any(l.split(" ", 1)[0] == agent_id for l in lines):
            return  # already recorded -- a second SubagentStart must not double-count
        lines.append(f"{agent_id} {int(time.time())}")
        _write_lines(fd, lines)
    _with_lock(path, fn)


def cmd_stop(path, agent_id):
    def fn(fd):
        lines = _read_lines(fd)
        kept = [l for l in lines if l.split(" ", 1)[0] != agent_id]
        _write_lines(fd, kept)
    _with_lock(path, fn)


def cmd_count(path, max_age_s):
    now = time.time()

    def fn(fd):
        fresh = 0
        for l in _read_lines(fd):
            parts = l.split(" ", 1)
            if len(parts) != 2:
                continue
            try:
                ts = float(parts[1])
            except ValueError:
                continue
            if now - ts <= max_age_s:
                fresh += 1
        return fresh
    return _with_lock(path, fn)


def main(argv):
    if len(argv) < 2:
        print("usage: agent-ledger.py start|stop <path> <agent_id>", file=sys.stderr)
        print("       agent-ledger.py count <path> [max_age_s]", file=sys.stderr)
        return 2
    action, path = argv[0], argv[1]
    try:
        if action == "start":
            if len(argv) < 3 or not argv[2]:
                return 0  # no agent_id to record -- never block on this
            cmd_start(path, argv[2])
        elif action == "stop":
            if len(argv) < 3 or not argv[2]:
                return 0
            cmd_stop(path, argv[2])
        elif action == "count":
            max_age = int(argv[2]) if len(argv) > 2 else MAX_AGE_S
            print(cmd_count(path, max_age))
        else:
            print(f"agent-ledger.py: unknown action {action!r}", file=sys.stderr)
            return 2
    except OSError as e:
        print(f"agent-ledger.py: {action} failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
