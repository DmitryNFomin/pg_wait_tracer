#!/usr/bin/env python3
"""test_workload_verify_flag.py -- unit test for Workload.fire()'s verify
flag (#243). Pure Python: fakes the psql sessions and the module-level
psql() one-shot backend spawner, no live PostgreSQL, no subprocess, no BPF
-- runs on the Mac.

The bug: fire() unconditionally spawned a fresh one-shot psql backend
(the module-level psql() helper -- a full connect/query/exit subprocess)
to assert the waiter blocked. tests/live_loop_workload.py calls fire()
every ~5-8s for up to 900s to keep Lock:relation/Timeout:PgSleep appearing
every live tick (#93), so on a live capture the Sessions tab counted ~150
distinct PIDs against ~8 real workload sessions (pgbench -c 4 plus
Workload's persistent holder/waiter/sleeper) -- almost all one-shot
status-check backends from this one call (#243 diagnostic walk,
tests/results/demo_rehearsal/tab_coverage.json on branch
agent/diag-walk-239: "session rows: 151").

This test cannot see the Sessions tab (that needs a live capture on
Linux). What it CAN pin, on any machine, without a database: whether
fire() invokes the one-shot backend spawner at all -- that call is the
entire mechanism of the leak, and it is deterministic pure-Python logic
(no timing, no ordering dependency: the fake psql/check/sleep are
substituted for the whole fire() call and every substitution is restored
in a `finally`, so this cannot pass by accident from FakeSession/FakeStdin
being no-ops -- the assertions count actual calls, not construct success).

Usage: python3 tests/test_workload_verify_flag.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_capture_smoke as tcs

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


class FakeStdin:
    def __init__(self):
        self.writes = []

    def write(self, s):
        self.writes.append(s)

    def flush(self):
        pass


class FakeSession:
    def __init__(self):
        self.stdin = FakeStdin()


def _make_workload():
    wl = tcs.Workload()
    wl.sleeper = FakeSession()
    wl.waiter = FakeSession()
    return wl


def _fire_with_fakes(verify):
    """Run Workload.fire() with psql()/check()/time.sleep() substituted for
    the whole call, always restored. Returns the list of SQL strings the
    one-shot psql() spawner was invoked with."""
    calls = []
    orig_psql, orig_check, orig_sleep = tcs.psql, tcs.check, time.sleep
    tcs.psql = lambda sql, timeout=15: (calls.append(sql), "1")[1]
    tcs.check = lambda cond, msg: None
    time.sleep = lambda s: None
    try:
        wl = _make_workload()
        wl.fire(sleep_s=3, verify=verify)
    finally:
        tcs.psql, tcs.check, time.sleep = orig_psql, orig_check, orig_sleep
    return calls


def test_fire_verify_true_spawns_one_backend():
    calls = _fire_with_fakes(verify=True)
    check(len(calls) == 1,
          f"fire(verify=True) [the default, used by every one-shot "
          f"smoke-test call site] spawns exactly one status-check "
          f"backend via psql() (calls={len(calls)})")


def test_fire_verify_false_spawns_no_backend():
    calls = _fire_with_fakes(verify=False)
    check(len(calls) == 0,
          f"fire(verify=False) spawns NO one-shot status-check backend -- "
          f"this is the #243 fix, used by tests/live_loop_workload.py's "
          f"per-tick loop so a 900s demo window does not spawn ~150 "
          f"one-shot backends (calls={len(calls)})")


def test_fire_still_writes_sleeper_and_waiter_sql():
    # verify=False must not skip the actual observable-wait SQL -- only
    # the extra status-check backend. Otherwise Lock:relation/Timeout:
    # PgSleep (#93) would silently stop appearing.
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        wl = _make_workload()
        wl.fire(sleep_s=3, verify=False)
    finally:
        time.sleep = orig_sleep
    sleeper_sql = "".join(wl.sleeper.stdin.writes)
    waiter_sql = "".join(wl.waiter.stdin.writes)
    check("pg_sleep(3)" in sleeper_sql,
          f"fire(verify=False) still writes the sleeper's pg_sleep SQL "
          f"(got {sleeper_sql!r})")
    check(wl.LOCK_TABLE in waiter_sql,
          f"fire(verify=False) still writes the waiter's blocking SQL "
          f"(got {waiter_sql!r})")


def test_live_loop_workload_calls_fire_with_verify_false():
    # Pins the actual fix in the actual caller, not just the mechanism in
    # Workload: the loop that runs for the whole demo window must be the
    # one passing verify=False, not some unrelated call site. This is a
    # source-text check specifically because the object under test here
    # (the *behaviour of the long-running demo loop*) cannot otherwise be
    # observed without a live capture.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "live_loop_workload.py")
    with open(path) as f:
        src = f.read()
    check("wl.fire(sleep_s=3, verify=False)" in src,
          "live_loop_workload.py's per-tick fire() call passes verify=False")


def main():
    test_fire_verify_true_spawns_one_backend()
    test_fire_verify_false_spawns_no_backend()
    test_fire_still_writes_sleeper_and_waiter_sql()
    test_live_loop_workload_calls_fire_with_verify_false()
    print(f"\n{tests_passed}/{tests_run} passed")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
