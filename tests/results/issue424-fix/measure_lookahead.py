#!/usr/bin/env python3
"""What the #424 straddler lookahead costs the current-trace cache.

The integration agent reported no latency numbers for the lookahead, and the
#283 cache question it raises is concrete: a window now decodes blocks PAST
its right edge, and offers them to the cache. So measure, on the same fixture
tests/test_data_current_trace_cache.py uses:

  - blocks DECODED for one window, default horizon (300 s) vs 1 s
  - events CHARGED to the cache in each case
  - wall time of the query, n=7, median

Run from tests/.  Output is a table; no assertions — this is a measurement,
not a gate.
"""
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_data_current_trace_cache as T       # noqa: E402
from server_harness import ServerHarness        # noqa: E402

S = T.S
BASE = T.BASE
N = 7


def run(horizon_ns, trace_dir, tmp):
    err = os.path.join(tmp, "m_%s.err" % horizon_ns)
    env = {"PGWT_CURRENT_TRACE_CACHE_STATS": "1",
           "PGWT_CURRENT_TRACE_CACHE": "1",
           "PGWT_STRADDLE_LOOKAHEAD_NS": str(horizon_ns)}
    lat = []
    stats = None
    with ServerHarness(trace_dir, env=env, stderr_path=err) as srv:
        # cold: this is the decode that the lookahead changes
        t0 = time.perf_counter()
        srv.query(T.PROBE_CMD, from_=str(BASE - 1 * S), to_=str(BASE + 4 * S))
        cold_ms = (time.perf_counter() - t0) * 1e3
        stats = T.read_curcache(err)
        for _ in range(N):
            t0 = time.perf_counter()
            srv.query(T.PROBE_CMD, from_=str(BASE - 1 * S),
                      to_=str(BASE + 4 * S))
            lat.append((time.perf_counter() - t0) * 1e3)
    return cold_ms, statistics.median(lat), stats


def main():
    import tempfile
    trace_dir = T.big_fixture()
    tmp = tempfile.mkdtemp(prefix="pgwt_measure_")
    print("fixture: %s" % trace_dir)
    print("window:  [BASE-1s, BASE+4s]   command: %s   n=%d warm queries"
          % (T.PROBE_CMD, N))
    print()
    print("%-14s %10s %12s %10s %10s %9s"
          % ("horizon", "cold ms", "warm ms md", "decoded", "events", "resets"))
    rows = {}
    for label, h in (("1 ms", 1_000_000), ("1 s", S), ("300 s (default)",
                                                       300 * S)):
        cold, warm, st = run(h, trace_dir, tmp)
        rows[label] = (cold, warm, st)
        print("%-14s %10.1f %12.1f %10s %10s %9s"
              % (label, cold, warm, st["decoded"], st["events"], st["resets"]))
    print()
    base = rows["1 ms"]
    deflt = rows["300 s (default)"]
    print("decode amplification, default vs 1 ms horizon: %sx blocks "
          "(%s -> %s), %sx events charged (%s -> %s)"
          % (round(deflt[2]["decoded"] / max(base[2]["decoded"], 1), 2),
             base[2]["decoded"], deflt[2]["decoded"],
             round(deflt[2]["events"] / max(base[2]["events"], 1), 2),
             base[2]["events"], deflt[2]["events"]))
    print("cold query delta: %+.1f ms;  warm median delta: %+.1f ms"
          % (deflt[0] - base[0], deflt[1] - base[1]))
    print()
    print("BOUND: cur_cache_store() charges against the SHARED budget before "
          "allocating (src/server.c: cache_total_events + n > CACHE_MAX_EVENTS "
          "|| cc->count + n > cur_cache_max_events() -> drop/decline), so the "
          "lookahead raises cache PRESSURE but cannot exceed the cap that was "
          "already there. The per-load decode bound is "
          "PGWT_STRADDLE_MAX_BLOCKS (256) and the time bound is "
          "PGWT_STRADDLE_LOOKAHEAD_NS (300 s).")


if __name__ == "__main__":
    main()
