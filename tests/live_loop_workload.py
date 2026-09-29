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

Issue #214 extended this: the original loop guarantees only two wait
classes, which is enough for the §2 conservation floors but leaves several
UI tabs structurally empty on a real rehearsal walk (Waterfall, Scatter --
see docs/DEMO_REHEARSAL_CRITERIA.md's workload section and
tests/demo_workload_coverage.py for the tab-by-tab table and the
machine-checkable condition each one needs). Two more persistent sessions
are added alongside holder/waiter/sleeper -- NOT inside them, so the
original Lock:relation / Timeout:PgSleep guarantee is untouched:

  - `reporter`: rotates through four structurally distinct, realistic
    SELECTs every tick (a catalog lookup, a CPU-bound aggregate, and two
    differently-shaped pg_sleep calls) -- gives Queries/Histogram/Waterfall/
    Scatter more than one query_id and a real spread of durations instead
    of the single ~3s pg_sleep the original loop produces alone.
  - `row_holder` / `row_waiter`: a hot-row UPDATE contended by two backends.
    row_waiter's UPDATE targets a row row_holder's OWN open transaction has
    already modified, so PostgreSQL makes it wait on row_holder's XID
    (XactLockTableWait -> Lock:transactionid) until row_holder commits --
    the standard "two sessions incrementing the same counter row" wait,
    realistic because hot-row contention on a shared counter/status row is
    one of the most common real-world lock waits (it is also
    tests/mock_server.py's own worked example for a waiter/blocker pair).
    It is a SECOND, DISTINCT wait class from the table-level Lock:relation
    the original loop already covers, which is what gives Transitions/
    Matrix more than one non-idle edge.

Usage: python3 tests/live_loop_workload.py DURATION_S
(SIGTERM stops it early and cleanly, same as any other loop in this suite.)
"""
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_capture_smoke import Workload, psql

ROW_LOCK_TABLE = "_smoke_row_lock_wait"

# Reporter's rotating query shapes (issue #214). Each is a DIFFERENT shape
# (target list / function calls), not just a different literal -- pg's
# query-id jumbling normalizes literals but not shape (see
# Workload.open_extra_session's own docstring), so each gets its own
# query_id, which is what Queries/Matrix/Waterfall/Scatter need to show
# more than one row. Durations span two orders of magnitude on purpose
# (Histogram needs "a spread, not one mode"; Waterfall needs "at least one
# slow enough to be interesting"):
#   catalog lookup   -- sub-ms to a few ms: a real app runs plenty of these
#   cpu aggregate     -- tens to a few hundred ms of genuine CPU work: the
#                        realistic stand-in for a report/aggregation query
#   short pg_sleep    -- ~0.4s: a fast dependent call (e.g. a synchronous
#                        downstream RPC) modeled as PgSleep, same class the
#                        original loop already guarantees every tick
#   long pg_sleep     -- ~1.3s: the "at least one slow enough to be
#                        interesting" execution Waterfall's own acceptance
#                        criterion (issue #214) names explicitly
REPORTER_QUERIES = (
    "SELECT count(*) FROM pg_class;",
    "SELECT count(*) FROM generate_series(1, 3000000);",
    "SELECT pg_sleep(0.4), 1;",
    "SELECT pg_sleep(1.3), 2, 3;",
)
# Sleep budget the main loop waits after sending each reporter query, so the
# next tick's SQL is never sent while the previous one is still running.
# Matches REPORTER_QUERIES order; generous over the expected runtime so a
# slower box (a loaded CI runner, an el8/el9 box) still finishes in time.
REPORTER_BUDGETS_S = (0.5, 2.0, 1.0, 2.0)


def _reporter_tick(reporter, iteration):
    idx = iteration % len(REPORTER_QUERIES)
    reporter.stdin.write(REPORTER_QUERIES[idx] + "\n")
    reporter.stdin.flush()
    time.sleep(REPORTER_BUDGETS_S[idx])


def _row_lock_tick(row_holder, row_waiter):
    """One Lock:transactionid cycle on a hot row -- realistic stand-in for a
    shared counter/status row under concurrent update (issue #214: "each
    added wait class needs a reason it is realistic")."""
    row_holder.stdin.write(
        f"BEGIN; UPDATE {ROW_LOCK_TABLE} SET v = v + 1 WHERE id = 1;\n")
    row_holder.stdin.flush()
    time.sleep(0.3)   # let row_holder's update land before row_waiter tries
    # Autocommit (no BEGIN): row_waiter's UPDATE hits the same row
    # row_holder's open transaction already modified, so it blocks on
    # row_holder's XID (Lock:transactionid) until row_holder commits below,
    # then this statement's own implicit transaction commits on its own.
    row_waiter.stdin.write(
        f"UPDATE {ROW_LOCK_TABLE} SET v = v + 1 WHERE id = 1;\n")
    row_waiter.stdin.flush()
    time.sleep(1.0)   # let the Lock:transactionid wait accumulate
    row_holder.stdin.write("COMMIT;\n")
    row_holder.stdin.flush()
    time.sleep(0.3)   # let row_waiter's statement land and auto-commit


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

    # Two extra persistent sessions (issue #214), alongside -- not inside --
    # the holder/waiter/sleeper trio above. open_extra_session() sends its
    # SQL immediately at creation, which is right for a fire-once session
    # but wrong here (reporter/row_holder/row_waiter are each sent DIFFERENT
    # SQL on every tick for the whole run), so these are opened directly via
    # Workload's own session-creation contract instead: a plain psql pipe
    # tagged under the same tag_base, reaped by the same stop() call because
    # it matches PGAPPNAME LIKE '{tag_base}%'.
    reporter = wl._session("reporter")
    row_holder = wl._session("row_holder")
    row_waiter = wl._session("row_waiter")
    wl.extra_sessions.extend([reporter, row_holder, row_waiter])

    psql(f"CREATE TABLE IF NOT EXISTS {ROW_LOCK_TABLE} (id int, v int)")
    psql(f"INSERT INTO {ROW_LOCK_TABLE} (id, v) "
         f"SELECT 1, 0 WHERE NOT EXISTS "
         f"(SELECT 1 FROM {ROW_LOCK_TABLE} WHERE id = 1)")
    time.sleep(0.5)   # let the extra sessions connect before the loop starts

    deadline = time.monotonic() + duration_s
    iteration = 0
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
            # verify=False: fire()'s default "waiter actually blocked" check
            # spawns a fresh one-shot psql backend. Fine for a smoke test
            # calling fire() a few times, but this loop calls fire() every
            # ~5-8s for the WHOLE demo window (up to 900s) -- at verify=True
            # that repeated one-shot backend was the dominant source of the
            # ~150 distinct PIDs polluting the Sessions tab in a demo
            # workload with ~8 real sessions (#243). See fire()'s docstring
            # for why skipping it here does not remove the real regression
            # guard.
            wl.fire(sleep_s=3, verify=False)
            _reporter_tick(reporter, iteration)
            _row_lock_tick(row_holder, row_waiter)
            time.sleep(2)
            wl.release()
            time.sleep(1)
            iteration += 1
    finally:
        # wl.stop() reaps reporter/row_holder/row_waiter too (extra_sessions,
        # matched by PGAPPNAME LIKE '{tag_base}%') and drops wl.LOCK_TABLE.
        # ROW_LOCK_TABLE is issue #214's own table -- drop it here rather than
        # leaving it behind for the next run (mirrors wl.LOCK_TABLE's default
        # keep_table=False hygiene).
        wl.stop(keep_table=False)
        psql(f"DROP TABLE IF EXISTS {ROW_LOCK_TABLE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
