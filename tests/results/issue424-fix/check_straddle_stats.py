#!/usr/bin/env python3
"""Does PGWT_STRADDLE_STATS=1 actually put the off-wire straddler counters
somewhere an operator can read them?

The review's point was that `straddlers_admitted` and `headers_unreadable`
were accumulated and never surfaced, so calling them a tripwire was a claim
with nothing behind it. This checks the claim rather than restating it.

Run from tests/:  python3 results/issue424-fix/check_straddle_stats.py
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, TESTS)

import test_data_current_trace_cache as T          # noqa: E402
from server_harness import ServerHarness           # noqa: E402


def main():
    trace_dir = T.big_fixture()
    tmp = tempfile.mkdtemp(prefix="pgwt_stats_")
    err = os.path.join(tmp, "stats.err")
    with ServerHarness(trace_dir, env={"PGWT_STRADDLE_STATS": "1"},
                       stderr_path=err) as srv:
        srv.query("top_events", from_=str(T.BASE - 1 * T.S),
                  to_=str(T.BASE + 4 * T.S))
    lines = [l.rstrip() for l in open(err) if "pgwt-straddle" in l]
    print("with PGWT_STRADDLE_STATS=1: %d line(s)" % len(lines))
    for l in lines[:3]:
        print("   " + l)

    # And silent by default, so it is a diagnostic rather than log noise.
    err2 = os.path.join(tmp, "quiet.err")
    with ServerHarness(trace_dir, env={}, stderr_path=err2) as srv:
        srv.query("top_events", from_=str(T.BASE - 1 * T.S),
                  to_=str(T.BASE + 4 * T.S))
    quiet = [l for l in open(err2) if "pgwt-straddle" in l]
    print("without the env var:        %d line(s)" % len(quiet))
    ok = len(lines) >= 1 and len(quiet) == 0
    print("VERDICT: %s" % ("readable on demand, silent by default"
                           if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
