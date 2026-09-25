#!/usr/bin/env python3
"""live_loop_workload.py -- a LOOPING Lock:relation / Timeout:PgSleep
workload for a real daemon + real Go bridge session.

Factored out of tests/ui_live_smoke.sh's original inline heredoc (issue
#93) so tests/demo_rehearsal.sh (issue #157) can run the exact same
workload for a much longer window without a second copy drifting from the
first (CLAUDE.md "Extend, do not fork"). Reuses test_capture_smoke.py's
Workload class (the same holder/waiter/sleeper psql sessions that test
already proves out) but LOOPs fire()/release() for the whole run instead
of firing once, so Lock:relation and Timeout:PgSleep keep appearing in
every live tick, not just the first one.

Usage: python3 tests/live_loop_workload.py DURATION_S
(SIGTERM stops it early and cleanly, same as any other loop in this suite.)
"""
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_capture_smoke import Workload


def main():
    if len(sys.argv) != 2:
        print("Usage: live_loop_workload.py DURATION_S", file=sys.stderr)
        return 2
    duration_s = float(sys.argv[1])
    stop = {"flag": False}

    def _stop(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, _stop)

    wl = Workload()
    wl.open_sessions()
    deadline = time.monotonic() + duration_s
    try:
        while not stop["flag"] and time.monotonic() < deadline:
            # Workload.release() COMMITs the holder, permanently dropping the
            # lock it took in open_sessions() -- a second fire() without
            # re-acquiring it would have the waiter sail through with no
            # Lock:relation wait at all. Re-issue the same BEGIN/LOCK
            # open_sessions() used, then fire()/release() as normal.
            wl.holder.stdin.write(
                f"BEGIN; LOCK TABLE {wl.LOCK_TABLE} IN ACCESS EXCLUSIVE MODE;\n")
            wl.holder.stdin.flush()
            time.sleep(0.5)   # let the re-lock land before the waiter tries
            wl.fire(sleep_s=3)
            time.sleep(2)
            wl.release()
            time.sleep(1)
    finally:
        wl.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
