#!/usr/bin/env python3
"""test_workload_verify_flag.py -- unit tests for Workload.fire()'s verify
flag and live_loop_workload.should_verify_tick()'s cadence (#243). Pure
Python: fakes the psql sessions and the module-level psql() one-shot
backend spawner, no live PostgreSQL, no subprocess, no BPF -- runs on the
Mac.

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

Review round 2: verify=False on EVERY tick (the first fix) means nothing
in any PR gate would notice the loop's own re-lock cycle silently
breaking -- so live_loop_workload.py now verifies periodically
(should_verify_tick(), every 10th tick) instead of never, and treats a
failed periodic verify as fatal. Two plausible wrong "periodic"
implementations exist -- never verify (ignores the cadence, same bug as
before) and verify every tick (ignores `every_n`, defeats the #243 fix) --
both are asserted against and demonstrated red below.

This test cannot see the Sessions tab or a live process exit (that needs
a live capture on Linux). What it CAN pin, on any machine, without a
database: whether fire() invokes the one-shot backend spawner at all, what
fire() returns, and whether should_verify_tick()'s cadence is real (not
"always" or "never") -- all deterministic pure-Python logic (no timing, no
ordering dependency: the fake psql/check/sleep are substituted for the
whole fire() call and every substitution is restored in a `finally`, so
this cannot pass by accident from FakeSession/FakeStdin being no-ops --
the assertions count actual calls and returned values, not construct
success).

Usage: python3 tests/test_workload_verify_flag.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_capture_smoke as tcs
import live_loop_workload as llw

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


def _fire_with_fakes(verify, waiters_response="1"):
    """Run Workload.fire() with psql()/check()/time.sleep() substituted for
    the whole call, always restored. waiters_response is what the faked
    one-shot psql() spawner returns for the "is the waiter blocked" query
    ("1" = blocked, "0" = not blocked, used to exercise fire()'s False
    return path). Returns (calls, result): calls is the list of SQL
    strings the spawner was invoked with; result is fire()'s return
    value."""
    calls = []
    orig_psql, orig_check, orig_sleep = tcs.psql, tcs.check, time.sleep
    tcs.psql = lambda sql, timeout=15: (calls.append(sql), waiters_response)[1]
    tcs.check = lambda cond, msg: None
    time.sleep = lambda s: None
    try:
        wl = _make_workload()
        result = wl.fire(sleep_s=3, verify=verify)
    finally:
        tcs.psql, tcs.check, time.sleep = orig_psql, orig_check, orig_sleep
    return calls, result


def test_fire_verify_true_spawns_one_backend():
    calls, result = _fire_with_fakes(verify=True)
    check(len(calls) == 1,
          f"fire(verify=True) [the default, used by every one-shot "
          f"smoke-test call site] spawns exactly one status-check "
          f"backend via psql() (calls={len(calls)})")


def test_fire_verify_false_spawns_no_backend():
    calls, result = _fire_with_fakes(verify=False)
    check(len(calls) == 0,
          f"fire(verify=False) spawns NO one-shot status-check backend -- "
          f"this is the #243 fix, used by tests/live_loop_workload.py's "
          f"per-tick loop so a 900s demo window does not spawn ~150 "
          f"one-shot backends (calls={len(calls)})")


def test_fire_verify_false_returns_true():
    # live_loop_workload.py's loop only treats a False return as fatal when
    # verify_this_tick was True -- verify=False must never itself produce a
    # False that would trip that check.
    _, result = _fire_with_fakes(verify=False)
    check(result is True,
          f"fire(verify=False) returns True (nothing was checked, so "
          f"nothing can be reported as failed) (got {result!r})")


def test_fire_verify_true_returns_true_when_blocked():
    _, result = _fire_with_fakes(verify=True, waiters_response="1")
    check(result is True,
          f"fire(verify=True) returns True when the waiter IS blocked "
          f"(got {result!r})")


def test_fire_verify_true_returns_false_when_not_blocked():
    # This is the return value live_loop_workload.py's periodic verify
    # relies on to fail loudly (sys.exit(1)) instead of looping on with a
    # silently-broken re-lock.
    _, result = _fire_with_fakes(verify=True, waiters_response="0")
    check(result is False,
          f"fire(verify=True) returns False when the waiter is NOT "
          f"blocked -- this is what live_loop_workload.py's fatal-exit "
          f"branch depends on (got {result!r})")


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


def test_fire_sleep_s_zero_skips_sleep_statement():
    # 2026-10-06: tests/live_loop_workload.py calls fire(sleep_s=0) so the
    # DEMO workload's slow query is the real waiter-blocked-on-holder
    # Lock:relation wait, not a manufactured pg_sleep (owner: a literal
    # pg_sleep(1.3) in the Top Queries panel reads as a faked demo). The
    # sleeper must get NO SQL at all; the waiter's blocking SQL -- what
    # actually produces Lock:relation -- stays unconditional.
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        wl = _make_workload()
        wl.fire(sleep_s=0, verify=False)
    finally:
        time.sleep = orig_sleep
    sleeper_sql = "".join(wl.sleeper.stdin.writes)
    waiter_sql = "".join(wl.waiter.stdin.writes)
    check(sleeper_sql == "",
          f"fire(sleep_s=0) writes NO SQL to the sleeper at all -- no "
          f"pg_sleep statement is sent (got {sleeper_sql!r})")
    check(wl.LOCK_TABLE in waiter_sql,
          f"fire(sleep_s=0) still writes the waiter's blocking SQL -- "
          f"the Lock:relation wait is unconditional (got {waiter_sql!r})")


def test_fire_default_sleep_s_still_sends_pg_sleep():
    # Pins the OTHER half of the contract: every existing one-shot
    # smoke-test call site (test_capture_smoke.py, test_query_event.py)
    # calls fire() with its default/explicit sleep_s=3 and must be totally
    # unaffected by the sleep_s=0 addition.
    wl = _make_workload()
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        wl.fire(sleep_s=3, verify=False)
    finally:
        time.sleep = orig_sleep
    sleeper_sql = "".join(wl.sleeper.stdin.writes)
    check("pg_sleep(3)" in sleeper_sql,
          f"fire() with the default sleep_s=3 still sends pg_sleep(3) "
          f"(got {sleeper_sql!r})")


def test_live_loop_workload_calls_fire_with_cadence():
    # Pins the actual fix in the actual caller, not just the mechanism in
    # Workload: the loop that runs for the whole demo window must pass
    # verify=<the cadence decision>, and must exit loudly when a verified
    # tick fails. This is a source-text check specifically because the
    # object under test here (the *behaviour of the long-running demo
    # loop*, including its fatal exit) cannot otherwise be observed
    # without a live capture.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "live_loop_workload.py")
    with open(path) as f:
        src = f.read()
    check("verify_this_tick = should_verify_tick(iteration)" in src,
          "live_loop_workload.py's loop computes verify_this_tick via "
          "should_verify_tick(), not a fixed True/False")
    check("wl.fire(sleep_s=0, verify=verify_this_tick)" in src,
          "live_loop_workload.py's per-tick fire() call passes the "
          "cadence decision, not a hardcoded verify= (sleep_s=0 since "
          "2026-10-06: the demo workload's slow query is the real "
          "Lock:relation wait, not a manufactured pg_sleep)")
    check("if verify_this_tick and not ok:" in src and "sys.exit(1)" in src,
          "live_loop_workload.py exits loudly (sys.exit(1)) when a "
          "verified tick finds the waiter not blocked")


# ── should_verify_tick() cadence (#243 review round 2) ─────────────────────
#
# Two plausible wrong "periodic" implementations, both demonstrated red
# against the assertions below (see the PR report for the actual red-run
# transcript): a should_verify_tick() that always returns False (silently
# reverts to "never verify", the exact regression this cadence exists to
# prevent) and one that always returns True (ignores `every_n`, silently
# reverts to "verify every tick", defeating the #243 Sessions-tab fix).

def test_should_verify_tick_fires_more_than_once():
    verified = [i for i in range(3 * llw.VERIFY_EVERY_N_TICKS)
                if llw.should_verify_tick(i)]
    # Catches "always False": a cadence that never verifies leaves this
    # list empty.
    check(len(verified) >= 2,
          f"should_verify_tick: verifies more than once over "
          f"{3 * llw.VERIFY_EVERY_N_TICKS} ticks (got {verified}) -- "
          f"catches a cadence that silently never verifies")


def test_should_verify_tick_includes_tick_zero():
    check(llw.should_verify_tick(0) is True,
          "should_verify_tick(0) is True -- a run shorter than "
          "VERIFY_EVERY_N_TICKS still verifies at least once")


def test_should_verify_tick_skips_most_ticks():
    verified = [i for i in range(3 * llw.VERIFY_EVERY_N_TICKS)
                if llw.should_verify_tick(i)]
    total = 3 * llw.VERIFY_EVERY_N_TICKS
    # Catches "always True": a cadence that verifies every tick defeats
    # the #243 fix (every tick spawns a one-shot backend again).
    check(len(verified) < total,
          f"should_verify_tick: does NOT verify every tick over {total} "
          f"ticks (got {verified}) -- catches a cadence that silently "
          f"verifies every tick (defeats the #243 Sessions-tab fix)")


def test_should_verify_tick_matches_every_n():
    every_n = llw.VERIFY_EVERY_N_TICKS
    expected = [i for i in range(3 * every_n) if i % every_n == 0]
    actual = [i for i in range(3 * every_n) if llw.should_verify_tick(i)]
    check(actual == expected,
          f"should_verify_tick: verifies exactly tick 0 and every "
          f"{every_n}th tick after (expected {expected}, got {actual})")


def test_verify_every_n_ticks_is_small_fraction():
    # Reviewer's own bound: keep the periodic backends a small fraction of
    # the real session count, not tuned to a precise floor -- just sane.
    check(2 <= llw.VERIFY_EVERY_N_TICKS <= 50,
          f"VERIFY_EVERY_N_TICKS ({llw.VERIFY_EVERY_N_TICKS}) is in a "
          f"sane range: too low re-introduces the #243 churn, too high "
          f"means a broken re-lock loop runs for a long time before "
          f"anything notices")


def test_advisory_rotation_covers_four_distinct_lock_shapes():
    # 2026-10-07 owner scope addition ("add more load"): four advisory-hold
    # shapes, not two -- pin that _reporter_tick's rotation actually
    # reaches all four, each with its own lock id and distinct
    # trailing-column count (what drives a distinct query_id).
    reporter = FakeSession()
    adv_holder = FakeSession()
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        for i in range(len(llw.REPORTER_QUERIES)):
            llw._reporter_tick(reporter, adv_holder, i)
    finally:
        time.sleep = orig_sleep
    adv_holder_sql = "".join(adv_holder.stdin.writes)
    reporter_sql = "".join(reporter.stdin.writes)
    for lock_id in (42, 43, 44, 45):
        check(f"pg_advisory_lock({lock_id})" in adv_holder_sql,
              f"adv_holder acquires lock id {lock_id} somewhere in one "
              f"full rotation")
    for trailing in ("1", "2, 3", "4, 5, 6", "7, 8, 9, 10"):
        check(f", {trailing};" in reporter_sql,
              f"reporter's own call carries trailing columns {trailing!r} "
              f"somewhere in one full rotation")


def test_advisory_hold_durations_are_the_requested_spread():
    check(llw.ADVISORY_HOLD_S[2:] == (0.1, 0.3, 1.0, 3.0),
          f"the four advisory hold durations are exactly 0.1/0.3/1.0/3.0s "
          f"(got {llw.ADVISORY_HOLD_S[2:]})")


def test_advisory_lock_ids_are_distinct():
    ids = [i for i in llw.ADVISORY_LOCK_IDS if i is not None]
    check(len(ids) == len(set(ids)),
          f"all four advisory lock ids are distinct (got {ids})")


def test_io_load_read_tick_sends_exact_batch_size_no_sleep():
    io_reader = FakeSession()
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        llw._io_load_read_tick(io_reader)
    finally:
        time.sleep = orig_sleep
    sql = "".join(io_reader.stdin.writes)
    count = sql.count(f"FROM {llw.IO_LOAD_TABLE}")
    check(count == llw.IO_LOAD_READS_PER_TICK,
          f"_io_load_read_tick sends exactly IO_LOAD_READS_PER_TICK "
          f"({llw.IO_LOAD_READS_PER_TICK}) reads (got {count})")
    check("pg_sleep" not in sql,
          f"_io_load_read_tick writes no server-side pg_sleep")


def test_io_load_write_tick_sends_exact_batch_size_no_sleep():
    io_writer = FakeSession()
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        llw._io_load_write_tick(io_writer)
    finally:
        time.sleep = orig_sleep
    sql = "".join(io_writer.stdin.writes)
    count = sql.count(f"UPDATE {llw.IO_LOAD_TABLE}")
    check(count == llw.IO_LOAD_WRITES_PER_TICK,
          f"_io_load_write_tick sends exactly IO_LOAD_WRITES_PER_TICK "
          f"({llw.IO_LOAD_WRITES_PER_TICK}) updates (got {count})")
    check("pg_sleep" not in sql,
          f"_io_load_write_tick writes no server-side pg_sleep")


def test_lockmgr_tick_fires_every_session_with_exact_repeat_count():
    sessions = [FakeSession() for _ in range(llw.LOCKMGR_SESSION_COUNT)]
    orig_sleep = time.sleep
    time.sleep = lambda s: None
    try:
        llw._lockmgr_tick(sessions)
    finally:
        time.sleep = orig_sleep
    for i, sess in enumerate(sessions):
        sql = "".join(sess.stdin.writes)
        count = sql.count(f"FROM {llw.LOCKMGR_TABLE}")
        check(count == llw.LOCKMGR_QUERIES_PER_TICK,
              f"lockmgr session {i} gets exactly LOCKMGR_QUERIES_PER_TICK "
              f"({llw.LOCKMGR_QUERIES_PER_TICK}) fanout queries "
              f"(got {count})")
        check("val > 0" in sql,
              f"lockmgr session {i}'s query filters on val, not the "
              f"partition key id (the pruning-defeating predicate)")


def test_lockmgr_query_does_not_filter_on_partition_key():
    where_clause = llw.LOCKMGR_QUERY.split("WHERE", 1)[1]
    check("id" not in where_clause,
          f"LOCKMGR_QUERY's WHERE clause does not reference id (the "
          f"partition key) -- filtering on it would let the planner prune "
          f"partitions instead of touching all {llw.LOCKMGR_PARTITIONS} "
          f"(got {llw.LOCKMGR_QUERY!r})")


def test_lockmgr_sessions_disable_parallel_workers():
    # 2026-10-07 regression (live, gate-1 box-check): LOCKMGR_QUERY's
    # COUNT(*) over a 200-partition table is a parallel-query candidate --
    # EXPLAIN confirmed "Gather Workers Planned: 2" -- so the 20x8=160
    # fanout executions per tick spawned ~320 extra forked parallel
    # workers/tick, which overwhelmed the daemon's backend tracking over a
    # full live-ui-smoke walk (pgwt-server pegged at CPU, every tab after
    # the first timed out). This is a source-text check (the SET is sent
    # once at session setup in main(), not inside a standalone testable
    # tick function) -- same pattern as
    # test_live_loop_workload_calls_fire_with_cadence above.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "live_loop_workload.py")
    with open(path) as f:
        src = f.read()
    check("max_parallel_workers_per_gather = 0" in src,
          "live_loop_workload.py disables parallel workers for the "
          "lockmgr sessions, so the fast-path-overflow mechanism does not "
          "also spawn a parallel-worker fork storm")



# ── should_lock_tick() cadence + the SHORT hold (owner 2026-10-08) ─────────
#
# The bug these pin: the loop used to take an ACCESS EXCLUSIVE table lock on
# EVERY tick and release it at the END of the tick, so the waiter was blocked
# for the whole 6-9s tick with no gap. Measured over a 900s capture that made
# Lock:relation 71% of DB Time -- against 0.1% for LWLock:LockManager and
# 0.2% for IO:DataFileRead. The owner's words: "pgsleep or lock relation
# looks very weird, So fix this."
#
# Two halves, two kinds of check. The CADENCE is a pure function, so it is
# tested directly (and every plausible wrong implementation is run against
# the same assertions in test_lock_cadence_mutants_are_caught below). The
# HOLD LENGTH is an ordering property of the loop body -- release() before
# the rest of the tick, not after -- which cannot be observed without a live
# capture, so it is pinned on the source text, same as
# test_live_loop_workload_calls_fire_with_cadence above.

def _lock_ticks(fn, n):
    return [i for i in range(n) if fn(i)]


def test_should_lock_tick_fires_more_than_once():
    span = 3 * llw.LOCK_EVERY_N_TICKS
    locked = _lock_ticks(llw.should_lock_tick, span)
    # Catches "never lock": REQUIRED_WORKLOAD_EVENTS names Lock:relation, so
    # a cadence that stops producing it fails the rehearsal verdict.
    check(len(locked) >= 2,
          f"should_lock_tick: locks more than once over {span} ticks "
          f"(got {locked}) -- catches a cadence that silently never locks")


def test_should_lock_tick_includes_tick_zero():
    # A DURATION_MIN=3 self-test is fewer than LOCK_EVERY_N_TICKS ticks; if
    # tick 0 did not lock, such a run would produce no Lock:relation at all
    # and demo_rehearsal_lib's workload_signature_present_ok would fail for
    # a reason that has nothing to do with the product.
    check(llw.should_lock_tick(0),
          "should_lock_tick(0) is True, so even a run shorter than "
          "LOCK_EVERY_N_TICKS ticks produces one Lock:relation wait")


def test_should_lock_tick_skips_most_ticks():
    span = 10 * llw.LOCK_EVERY_N_TICKS
    locked = _lock_ticks(llw.should_lock_tick, span)
    # THE assertion for the 71% bug: "lock on every tick" is a cadence, and
    # it is the one that was shipped. Anything above half the ticks is not
    # "occasional" by any reading.
    check(len(locked) * 2 <= span,
          f"should_lock_tick: locks on {len(locked)}/{span} ticks, i.e. at "
          f"most half -- catches the shipped regression where the explicit "
          f"table lock was taken every single tick (71% of DB Time)")


def test_should_lock_tick_matches_every_n():
    span = 4 * llw.LOCK_EVERY_N_TICKS + 3
    expected = [i for i in range(span) if i % llw.LOCK_EVERY_N_TICKS == 0]
    actual = _lock_ticks(llw.should_lock_tick, span)
    check(actual == expected,
          f"should_lock_tick matches every {llw.LOCK_EVERY_N_TICKS}th tick "
          f"exactly (expected {expected}, got {actual})")


def test_should_lock_tick_honours_every_n_argument():
    # Catches an implementation that ignores `every_n` and hardcodes the
    # module constant -- which would make the mutant table below vacuous.
    check(_lock_ticks(lambda i: llw.should_lock_tick(i, 3), 10)
          == [0, 3, 6, 9],
          "should_lock_tick honours an explicit every_n instead of always "
          "using LOCK_EVERY_N_TICKS")


def test_lock_cadence_mutants_are_caught():
    # The bypass direction: every plausible wrong implementation of "lock
    # occasionally" must FAIL at least one assertion above. A mutant that
    # satisfies all of them would mean those assertions cannot see it.
    span_small = 3 * llw.LOCK_EVERY_N_TICKS
    span_big = 10 * llw.LOCK_EVERY_N_TICKS
    mutants = {
        # The shipped regression: lock on every tick.
        "always True (lock every tick -- the 71% bug)":
            lambda i, every_n=None: True,
        # The opposite failure: never lock, Lock:relation disappears.
        "always False (never lock)":
            lambda i, every_n=None: False,
        # Off by one: never fires on tick 0, so a short run produces no
        # Lock:relation at all even though the cadence "looks periodic".
        "off-by-one (i % n == 1)":
            lambda i, every_n=llw.LOCK_EVERY_N_TICKS: i % every_n == 1,
    }
    for label, fn in mutants.items():
        locked_small = _lock_ticks(fn, span_small)
        locked_big = _lock_ticks(fn, span_big)
        caught = (len(locked_small) < 2                      # fires once
                  or not fn(0)                               # misses tick 0
                  or len(locked_big) * 2 > span_big          # too often
                  or locked_big != [i for i in range(span_big)
                                    if i % llw.LOCK_EVERY_N_TICKS == 0])
        check(caught,
              f"lock-cadence mutant {label!r} is rejected by at least one "
              f"of the assertions above")


def test_verify_ticks_are_all_lock_ticks():
    # A verify asks pg_locks whether the waiter is blocked on wl.LOCK_TABLE.
    # On a tick where no lock was taken the honest answer is "no", which the
    # loop treats as FATAL -- so a verify tick that is not a lock tick kills
    # a rehearsal partway through for no product reason.
    span = 5 * llw.VERIFY_EVERY_N_TICKS
    stray = [i for i in range(span)
             if llw.should_verify_tick(i) and not llw.should_lock_tick(i)]
    check(not stray,
          f"every verify tick over {span} ticks is also a lock tick "
          f"(stray verify-without-lock ticks: {stray})")


def test_verify_tick_is_lock_tick_predicate():
    # The predicate the module asserts at import. Table-driven, including the
    # degenerate inputs: a zero cadence must be REFUSED, not approved -- an
    # accidental truth there is exactly how a guard ends up approving
    # nonsense it cannot evaluate.
    cases = [
        ((10, 5), True,  "the shipped pair: 10 is a multiple of 5"),
        ((10, 10), True, "equal cadences: every verify tick is a lock tick"),
        ((10, 1), True,  "lock every tick is compatible (if wasteful)"),
        ((10, 3), False, "10 is not a multiple of 3 -- ticks 10, 20 would "
                         "verify a lock that was never taken"),
        ((10, 4), False, "10 is not a multiple of 4"),
        ((10, 0), False, "a zero lock cadence is refused, not approved"),
        ((0, 5), False,  "a zero verify cadence is refused, not approved"),
    ]
    for (v, l), expected, why in cases:
        got = llw.verify_tick_is_lock_tick(v, l)
        check(got is expected,
              f"verify_tick_is_lock_tick({v}, {l}) is {expected} -- {why} "
              f"(got {got})")


def test_shipped_constants_satisfy_the_invariant():
    check(llw.verify_tick_is_lock_tick(),
          f"the shipped VERIFY_EVERY_N_TICKS={llw.VERIFY_EVERY_N_TICKS} / "
          f"LOCK_EVERY_N_TICKS={llw.LOCK_EVERY_N_TICKS} pair satisfies the "
          f"invariant (the module also asserts this at import)")


def test_lock_cycle_is_guarded_and_released_before_the_rest_of_the_tick():
    # The HOLD-LENGTH half of the fix, pinned on source text because the
    # thing being asserted is an ordering inside a loop that only runs
    # against a live PostgreSQL.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "live_loop_workload.py")
    with open(path) as f:
        src = f.read()
    check("if should_lock_tick(iteration):" in src,
          "live_loop_workload.py's loop guards the explicit LOCK TABLE "
          "cycle behind should_lock_tick(), so it is occasional rather "
          "than every tick")
    i_lock = src.find("IN ACCESS EXCLUSIVE MODE")
    i_release = src.find("wl.release()", i_lock)
    i_reporter = src.find("_reporter_tick(reporter, adv_holder, iteration)",
                           i_lock)
    check(i_lock != -1 and i_release != -1 and i_reporter != -1,
          "live_loop_workload.py's loop still contains the LOCK TABLE "
          "statement, a wl.release() after it, and the reporter tick")
    check(i_release < i_reporter,
          "wl.release() comes BEFORE the rest of the tick's component "
          "ticks, so the Lock:relation hold is fire()'s own ~1.5s rather "
          "than the whole 6-9s tick -- the ordering that made Lock:relation "
          f"71% of DB Time (release at {i_release}, reporter tick at "
          f"{i_reporter})")


def main():
    test_fire_verify_true_spawns_one_backend()
    test_fire_verify_false_spawns_no_backend()
    test_fire_verify_false_returns_true()
    test_fire_verify_true_returns_true_when_blocked()
    test_fire_verify_true_returns_false_when_not_blocked()
    test_fire_still_writes_sleeper_and_waiter_sql()
    test_fire_sleep_s_zero_skips_sleep_statement()
    test_fire_default_sleep_s_still_sends_pg_sleep()
    test_live_loop_workload_calls_fire_with_cadence()
    test_should_verify_tick_fires_more_than_once()
    test_should_verify_tick_includes_tick_zero()
    test_should_verify_tick_skips_most_ticks()
    test_should_verify_tick_matches_every_n()
    test_verify_every_n_ticks_is_small_fraction()
    test_advisory_rotation_covers_four_distinct_lock_shapes()
    test_advisory_hold_durations_are_the_requested_spread()
    test_advisory_lock_ids_are_distinct()
    test_io_load_read_tick_sends_exact_batch_size_no_sleep()
    test_io_load_write_tick_sends_exact_batch_size_no_sleep()
    test_lockmgr_tick_fires_every_session_with_exact_repeat_count()
    test_lockmgr_query_does_not_filter_on_partition_key()
    test_lockmgr_sessions_disable_parallel_workers()
    test_should_lock_tick_fires_more_than_once()
    test_should_lock_tick_includes_tick_zero()
    test_should_lock_tick_skips_most_ticks()
    test_should_lock_tick_matches_every_n()
    test_should_lock_tick_honours_every_n_argument()
    test_lock_cadence_mutants_are_caught()
    test_verify_ticks_are_all_lock_ticks()
    test_verify_tick_is_lock_tick_predicate()
    test_shipped_constants_satisfy_the_invariant()
    test_lock_cycle_is_guarded_and_released_before_the_rest_of_the_tick()
    print(f"\n{tests_passed}/{tests_run} passed")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
