#!/usr/bin/env python3
"""test_multi_window_lib.py — the pure parsers behind test_multi_window.py.

No PostgreSQL, no root, no tracer: fixtures only. Exists because
test_multi_window's Test-3 conservation check is only as trustworthy as
what it parses, and the parser had a defect that MANUFACTURED positive
excess inside that very check (issue #202 review).

The defect. src/output.c prints the multi-window time model per tick and
OMITS a class row whose first-window value is 0
(`if (classes[i].sort_ns == 0) continue`, output.c ~line 241), and omits
Off-CPU* when no window has one (output.c ~line 197). So the row SET
changes from tick to tick. parse_first_window() used to scan the whole
output with last-writer-wins, which kept a class's tick-2 milliseconds
alongside a DB Time read from tick 3 — the identity was checked against a
mixture of two windows. Some unknown part of #202's reported 20.2%
(18330 ms vs 15255 ms) was this, not the accounting bug.

Usage: python3 tests/test_multi_window_lib.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_multi_window import parse_first_window, split_ticks

tests_run = 0
tests_passed = 0


def check(cond, msg):
    global tests_run, tests_passed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        print(f"  FAIL: {msg}")


# Two ticks in the exact shape src/output.c emits. Tick 1 has a Lock row
# and no Off-CPU*; tick 2 has neither Lock (it went to 0, so output.c drops
# the row) but does have Off-CPU*. Each tick is internally conservative:
#   tick 1:  600 + 300 + 100          = 1000 = DB Time
#   tick 2:  500 + 200 +  50 (Off-CPU) = 750 = DB Time
# Merged last-writer-wins you get DB Time 750 with Lock's 100 from tick 1
# still in the dict: 500 + 200 + 50 + 100 = 850 vs 750 = +13.3% excess out
# of thin air.
TWO_TICKS = """
--- 2026-09-28 00:00:01 ---
================================================================================
pg_wait_tracer — Time Model    Backends: 4    Interval: 5s
================================================================================

  Stat Name                          Last 5s    % DB
  -------------------------------- --------- -------
  DB Time                             1000.0  100.0%
    CPU*                               600.0   60.0%
    IO                                 300.0   30.0%
      IO:WalSync                       280.0   28.0%
    Lock                               100.0   10.0%
      Lock:transactionid                90.0    9.0%

  (Activity/Idle)                     5000.0       -


--- 2026-09-28 00:00:06 ---
================================================================================
pg_wait_tracer — Time Model    Backends: 4    Interval: 5s
================================================================================

  Stat Name                          Last 5s    % DB
  -------------------------------- --------- -------
  DB Time                              750.0  100.0%
    CPU*                               500.0   66.7%
    Off-CPU*                            50.0    6.7%
    IO                                 200.0   26.7%
      IO:WalSync                       180.0   24.0%

  (Activity/Idle)                     5200.0       -
"""

EXCLUDE = {'DB Time', 'Idle'}


def top_level(model):
    return {k: v for k, v in model.items()
            if ':' not in k and k not in EXCLUDE}


def test_single_tick():
    print("--- one tick, never a mixture of two ---")
    check(len(split_ticks(TWO_TICKS)) == 2,
          f"fixture splits into 2 ticks (got {len(split_ticks(TWO_TICKS))})")

    last = parse_first_window(TWO_TICKS)
    check(last.get('DB Time') == 750.0,
          f"the LAST tick's DB Time is parsed (got {last.get('DB Time')})")
    # The whole point: a row that tick 2 does not print must NOT be carried
    # over from tick 1. This is the assertion the old parser failed.
    check('Lock' not in last,
          f"a class tick 2 omitted is absent, not inherited from tick 1 "
          f"(rows {sorted(top_level(last))})")
    check(last.get('Off-CPU*') == 50.0,
          f"a row only tick 2 has is present (got {last.get('Off-CPU*')})")
    s = sum(top_level(last).values())
    check(abs(s - 750.0) < 0.05,
          f"tick 2 conserves: sum(top-level) = {s} vs DB Time 750.0")

    first = parse_first_window(TWO_TICKS, tick=0)
    check(first.get('DB Time') == 1000.0,
          f"tick 0 is reachable explicitly (got {first.get('DB Time')})")
    check(first.get('Lock') == 100.0 and 'Off-CPU*' not in first,
          f"…with tick 0's OWN row set (rows {sorted(top_level(first))})")
    s0 = sum(top_level(first).values())
    check(abs(s0 - 1000.0) < 0.05,
          f"tick 0 conserves too: sum(top-level) = {s0} vs DB Time 1000.0")

    # The number the old parser produced, spelled out, so the regression is
    # named rather than merely prevented.
    merged = dict(first)
    merged.update(last)
    bad = sum(top_level(merged).values())
    check(abs(bad - 850.0) < 0.05 and bad > 750.0,
          f"the merged-ticks parse would have read {bad}ms against a DB "
          f"Time of 750ms (+{100 * (bad - 750) / 750:.1f}%) — manufactured "
          f"excess, which is what this test exists to stop")


def test_cannot_see_refuses():
    """A parser that cannot see must return nothing, never a plausible dict.

    Each of these used to (or could) yield a dict that the caller would
    then happily 'verify', so they are asserted explicitly.
    """
    print("--- blind inputs yield {} , never a half-parsed model ---")
    check(parse_first_window("") == {},
          "empty output -> {} (Test 3 then fails on 'DB Time found')")
    check(parse_first_window("no tick separators here at all\n") == {},
          "output with no tick separator -> {}")
    # "waiting for data" is a REAL first tick: it has a separator but no
    # rows. It must parse to an empty model, not to a partial one.
    waiting = ("\n--- 2026-09-28 00:00:01 ---\n"
               "pg_wait_tracer — Time Model    Backends: 4    Interval: 5s\n"
               "  (waiting for data)\n")
    check(parse_first_window(waiting) == {},
          "a 'waiting for data' tick -> {} , not a partial model")
    # A tick whose DB Time row is present but whose classes are all absent
    # must still parse DB Time — absent rows are the caller's problem to
    # fail on, not something the parser should paper over.
    only_db = ("\n--- 2026-09-28 00:00:01 ---\n"
               "  DB Time                              750.0  100.0%\n")
    m = parse_first_window(only_db)
    check(m == {'DB Time': 750.0},
          f"a DB-Time-only tick parses to exactly that (got {m})")
    check(top_level(m) == {},
          "…and its top-level row set is empty, so Test 3 fails it loudly")


def main():
    test_single_tick()
    test_cannot_see_refuses()
    print(f"\n{tests_passed}/{tests_run} checks passed")
    return 0 if tests_passed == tests_run else 1


if __name__ == "__main__":
    sys.exit(main())
