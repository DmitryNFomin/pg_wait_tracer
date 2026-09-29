#!/usr/bin/env python3
"""Reap processes from one cancelled self-hosted CI cell on Linux."""

import os
from pathlib import Path
import signal
import sys
import time


def ancestors(proc_root):
    result = set()
    pid = os.getpid()
    while pid > 1 and pid not in result:
        result.add(pid)
        try:
            stat = (proc_root / str(pid) / "stat").read_text()
            pid = int(stat.rsplit(") ", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return result


def cell_pids(proc_root, token, excluded):
    marker = b"PGWT_CI_CELL=" + token.encode() + b"\0"
    found = set()
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid in excluded:
            continue
        try:
            if marker in (entry / "environ").read_bytes():
                found.add(pid)
        except OSError:
            pass  # Process exited during the scan.
    return found


def reap(proc_root, token):
    excluded = ancestors(proc_root)
    found = cell_pids(proc_root, token, excluded)
    if not found:
        print(f"gate cell {token}: no leftover processes")
        return True
    print(f"gate cell {token}: terminating leftover PIDs {sorted(found)}", flush=True)
    for pid in found:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        remaining = cell_pids(proc_root, token, excluded)
        if not remaining:
            return True
        time.sleep(0.2)
    remaining = cell_pids(proc_root, token, excluded)
    for pid in remaining:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    time.sleep(0.2)
    remaining = cell_pids(proc_root, token, excluded)
    if remaining:
        print(f"gate cell {token}: could not reap PIDs {sorted(remaining)}", file=sys.stderr)
        return False
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2 or not sys.argv[1]:
        sys.exit("usage: cleanup_gate_cell.py RUN_CELL_TOKEN")
    sys.exit(0 if reap(Path("/proc"), sys.argv[1]) else 1)
