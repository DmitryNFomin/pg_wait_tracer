#!/usr/bin/env python3
"""test_timeline_compare.py -- comparator of #1006 unit 1 (S5).

S0 placeholder, skip-free: the module imports and carries the frozen
interface and the amendment-6 thresholds. S5 replaces this with synthetic
exact, merged, split, missed and extra cases. Registered in scripts/check.sh.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import timeline_compare as tc  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if cond:
        print("  ok: %s" % what)
    else:
        fails += 1
        print("  FAIL: %s" % what)


for name in ("load_trap", "load_timeline", "compare", "evaluate", "main"):
    check(callable(getattr(tc, name, None)), "timeline_compare.%s is callable" % name)
check(tc.STRICT_ERR_NS == 50_000, "strict error bound is 50 us")
check(tc.THRESHOLDS == {"strict_wait_pct": 96.0, "overlap_time_pct": 96.0,
                        "extra_time_pct": 0.01, "pid_min_calls": 100},
      "amendment-6 thresholds")
r = tc.CompareResult(trap_waits=4, strict_matched=3, trap_wait_ns=1000, overlap_ns=500, extra_ns=1)
check(r.strict_wait_pct() == 75.0 and r.overlap_time_pct() == 50.0 and r.extra_time_pct() == 0.1,
      "CompareResult percentages")
check(tc.main([]) == 2, "main refuses without arguments")

if fails:
    print("test_timeline_compare: %d check(s) failed" % fails)
    sys.exit(1)
print("test_timeline_compare: all checks passed")
