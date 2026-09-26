#!/usr/bin/env python3
"""check-lock.py -- the actual mutual-exclusion primitive behind
scripts/check-lock.sh (machine-wide serialization of `make check`).

Review history: the first version of this mechanism (see git log) judged a
held lock file "stale" in userspace by recording the holder's pid plus a
`ps`-derived start-time fingerprint, and reclaimed a stale lock by
`mv`-ing a challenger's claim over it and reading the result back. That
reclaim was NOT atomic: an N-waiter experiment against one fabricated dead
lock reproduced double-holds (two waiters both believing they held the
lock at once) in roughly 15-30% of trials. `holder_alive()` also failed
OPEN on a `kill(pid, 0)` permission error or an empty `ps` fingerprint,
treating "we don't actually know" as "dead" -- exactly backwards for a
fail-closed lock.

This version replaces ALL of that userspace staleness logic with a real
kernel primitive: flock(2) (via Python's `fcntl.flock`, since macOS has no
`flock(1)` and `make check` already hard-requires python3). flock(2)'s
mutual exclusion is enforced by the kernel against the open file
description, so:
  - Two processes racing to acquire it: the kernel serializes them --
    there is no window to reason about, unlike a rename-then-read-back.
  - A holder killed outright (SIGKILL, this machine's memory manager):
    the kernel releases the lock the instant the process's last fd
    referencing it is closed, which the kernel itself guarantees on
    process death. No "is the holder still alive" heuristic is needed,
    so none of that heuristic's failure modes (pid reuse, a `kill -0`
    permission error, a `ps` hiccup) are even reachable anymore.

The lock file's CONTENTS play no role in correctness -- they are written
by the holder purely so a waiter's status message can name who holds it
and for how long. Garbage/stale content, or no content at all, changes
nothing about whether the OS-level lock is actually held.
"""
import errno
import fcntl
import getpass
import os
import socket
import subprocess
import sys
import time


def read_info(lock_path):
    """Best-effort, display-only: who does the lock file SAY holds it."""
    try:
        with open(lock_path) as f:
            data = f.read()
    except OSError:
        return "held by <unknown> (lock file unreadable)"
    fields = {}
    for line in data.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k] = v
    pid = fields.get("pid", "?")
    host = fields.get("host", "?")
    user = fields.get("user", "?")
    label = fields.get("label", "?")
    acquired_at = fields.get("acquired_at")
    if acquired_at:
        try:
            held = f"for {int(time.time() - float(acquired_at))}s"
        except ValueError:
            held = "for an unknown time"
    else:
        held = "for an unknown time"
    return f"held by {label} (pid {pid}, {user}@{host}) {held}"


def main(argv):
    if not argv:
        print("usage: check-lock.py <command> [args...]", file=sys.stderr)
        return 2

    lock_path = os.environ.get("PGWT_CHECK_LOCK_FILE", "/tmp/pgwt-check.lock")
    poll = float(os.environ.get("PGWT_CHECK_LOCK_POLL", "5"))
    notify_every = float(os.environ.get("PGWT_CHECK_LOCK_NOTIFY", "20"))
    cmd_desc = " ".join(argv)

    try:
        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o644)
    except OSError as e:
        print(f"check-lock: cannot open/create lock file '{lock_path}': {e} "
              f"-- refusing to run '{cmd_desc}' unlocked. "
              f"(humans only: PGWT_SKIP_CHECK_LOCK=1)", file=sys.stderr)
        return 2

    label = os.getcwd()
    try:
        branch = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5,
        ).stdout.strip()
        if branch:
            label += f" ({branch})"
    except Exception:
        pass

    last_print = 0.0
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError as e:
            if e.errno not in (errno.EACCES, errno.EAGAIN, errno.EWOULDBLOCK):
                print(f"check-lock: flock('{lock_path}') failed "
                      f"unexpectedly: {e} -- refusing to run "
                      f"'{cmd_desc}' unlocked.", file=sys.stderr)
                os.close(fd)
                return 2
        except KeyboardInterrupt:
            os.close(fd)
            return 130

        now = time.time()
        if now - last_print >= notify_every:
            print(f"check-lock: waiting to run '{cmd_desc}' -- this is "
                  f"expected with several agents on one Mac, not a hang. "
                  f"{read_info(lock_path)}; re-checking every {poll:g}s...",
                  file=sys.stderr)
            last_print = now
        try:
            time.sleep(poll)
        except KeyboardInterrupt:
            os.close(fd)
            return 130

    # We now hold the real OS-level lock. Metadata below is informational
    # only (see module docstring) -- it is never consulted to decide
    # whether the lock is held.
    try:
        os.ftruncate(fd, 0)
        os.lseek(fd, 0, os.SEEK_SET)
        meta = (
            f"pid={os.getpid()}\n"
            f"host={socket.gethostname()}\n"
            f"user={getpass.getuser()}\n"
            f"label={label}\n"
            f"cmd={cmd_desc}\n"
            f"acquired_at={int(time.time())}\n"
        )
        os.write(fd, meta.encode())
        os.fsync(fd)
    except OSError:
        pass  # informational only; never affects the lock itself

    try:
        rc = subprocess.call(argv)
    except FileNotFoundError as e:
        print(f"check-lock: cannot run '{cmd_desc}': {e}", file=sys.stderr)
        rc = 127
    finally:
        try:
            os.ftruncate(fd, 0)
        except OSError:
            pass
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)

    if rc < 0:
        # subprocess.call() returns -SIGNAL when the child died from a
        # signal (e.g. -9 for SIGKILL) -- normalise to the shell
        # convention (128 + signal number).
        rc = 128 - rc
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
