#!/usr/bin/env python3
"""Run a command in its own process group; mark actual deadline expiry."""
import os
from pathlib import Path
import signal
import subprocess
import sys


def stop_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    # Bound cleanup even if the command or one of its children ignores TERM.
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def main():
    seconds = float(sys.argv[1])
    marker = Path(sys.argv[2])
    process = subprocess.Popen(sys.argv[3:], start_new_session=True)

    def interrupted(signum, frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        return process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        stop_group(process)
        marker.touch()
        print('BUDGET_EXCEEDED: Codex wall-clock deadline reached; salvaging work', file=sys.stderr)
        return 124
    except BaseException:
        stop_group(process)
        raise


if __name__ == '__main__':
    sys.exit(main())
