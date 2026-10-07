#!/usr/bin/env python3
"""live_loop_workload.py -- a LOOPING Lock:relation / Lock:advisory
workload for a real daemon + real Go bridge session.

Factored out of tests/ui_live_smoke.sh's original inline heredoc (issue
#93) so tests/demo_rehearsal.sh (issue #157) can run the exact same
workload for a much longer window without a second copy drifting from the
first (CLAUDE.md "Extend, do not fork"). Reuses test_capture_smoke.py's
Workload class (the same holder/waiter/sleeper psql sessions that test
already proves out) but LOOPs fire()/release() for the whole run instead
of firing once, so Lock:relation keeps appearing in every live tick, not
just the first one. fire() is called with sleep_s=0 (owner decision
2026-10-06): a DBA watching the demo reads a literal `pg_sleep(1.3)` in
the Top Queries panel as a faked workload, while the waiter genuinely
blocking on the holder's lock is real, honest slow work -- so the
sleeper's pg_sleep statement is skipped entirely here; the blocking SQL on
the waiter (which is what actually produces Lock:relation) is
unconditional and unaffected. `pg_sleep` itself is untouched everywhere
else: it remains the known-quantity accuracy proof in
test_aas_accuracy.py, test_accuracy.py, test_deterministic.py,
test_query_accuracy.py and test_capture_smoke.py's own default-arg call
sites.

Issue #214 extended this: the original loop guarantees only one wait
class (Lock:relation), which is enough for the §2 conservation floors but
leaves several UI tabs structurally empty on a real rehearsal walk
(Waterfall, Scatter -- see docs/DEMO_REHEARSAL_CRITERIA.md's workload
section and tests/demo_workload_coverage.py for the tab-by-tab table and
the machine-checkable condition each one needs). More persistent sessions
are added alongside holder/waiter/sleeper -- NOT inside them, so the
original Lock:relation guarantee is untouched:

  - `reporter`: rotates through four structurally distinct, realistic
    SELECTs every tick (a catalog lookup, a CPU-bound aggregate, and two
    differently-shaped advisory-lock holds -- see `adv_holder` below) --
    gives Queries/Histogram/Waterfall/Scatter more than one query_id and a
    real spread of durations instead of the single Lock:relation wait the
    original loop produces alone.
  - `adv_holder`: a persistent session that holds `pg_advisory_lock(42)`
    for a CLIENT-SIDE `time.sleep()` (never a server-side `pg_sleep`,
    never `generate_series` -- its timing is CPU-dependent and the gate
    boxes have different silicon) while the reporter's own
    `pg_advisory_lock(42)` call blocks on it -> `Lock:advisory` of exactly
    the hold duration, reading as an application mutex rather than a
    manufactured sleep (owner 2026-10-06: a `pg_sleep(1.3)` visible in the
    Top Queries panel reads as a faked demo to any DBA in the room).
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
  - `io_reader`: added 2026-10-06 so the IO panels show real
    `IO:DataFileRead`/`IO:DataFileWrite` activity (owner: "we also need IO
    datafile read and write"), not because it changes the Transitions tab
    -- an initial version (a `SELECT count(*)` full sequential scan) did
    NOT add Transitions node variety (5 nodes with or without it, measured
    on two independent full box-check runs, 2026-10-07) and was a heavy
    ~150MB-ish scan every tick for no node-count benefit. Replaced
    2026-10-07 with an indexed single-row read on a random `aid`
    (pgbench_accounts has 1,000,000 rows): a primary-key lookup still
    produces a real `IO:DataFileRead` whenever that row's page is not in
    shared_buffers, at a small fraction of the scan's cost. No sleep, no
    CPU-dependent timing -- just a real statement against data already
    there.

#243 review round 2: the re-lock loop below calls fire() with verify=False
on most ticks (a one-shot psql backend per verify was the dominant source
of ~150 distinct Sessions-tab PIDs against ~8 real workload sessions -- see
Workload.fire()'s docstring), but NEVER is wrong: nothing in any PR gate
would then notice this loop's own BEGIN/LOCK TABLE re-lock silently
breaking (the smoke test's own verify=True call sites only ever exercise
open_sessions()'s one-shot lock, not this repeated cycle; live-UI-smoke
grades rendering, not which wait class produced it; demo_workload_coverage's
transitions/matrix checkers need any non-idle edge, not Lock:relation by
name; the one check that does name it is wired only into demo_rehearsal.py,
which never gates CI). should_verify_tick() below re-enables verify=True
periodically instead, and a failed periodic verify is FATAL (sys.exit(1)),
not print-and-continue -- ui_live_smoke.py's/demo_rehearsal.py's own
_assert_workload_alive liveness check (called at every tab boundary/pass)
then catches the dead workload process and fails the whole run loudly.

Usage: python3 tests/live_loop_workload.py DURATION_S
(SIGTERM stops it early and cleanly, same as any other loop in this suite.)
"""
import os
import random
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_capture_smoke import Workload, psql

# How many ticks between periodic re-verification of "the waiter actually
# blocked" on the LOOPING re-lock path (#243 review round 2). At ~5-8s/tick
# a 900s demo window is roughly 130 ticks; every 10th tick keeps the added
# one-shot backends (~13) a small fraction of the ~150 the #243 fix removed,
# while still re-proving the mechanism every ~50-80s of wall time -- a break
# would be caught well within a single rehearsal pass, not just "eventually,
# by hand" (the gap review round 2 found). Any N that keeps the periodic
# backends a small fraction of the real session count is fine; this is not
# tuned to a precise floor.
VERIFY_EVERY_N_TICKS = 10


def should_verify_tick(iteration, every_n=VERIFY_EVERY_N_TICKS):
    """True on tick 0 and every `every_n`th tick after. Pure (no I/O), so
    the CADENCE itself -- not just fire()'s verify flag -- is unit-testable
    without a live loop or database (tests/test_workload_verify_flag.py):
    a regression that never verifies (always False) or that ignores
    `every_n` and verifies every tick (always True) are both plausible
    wrong implementations of "verify periodically", and both are
    distinguishable from the real cadence over a handful of ticks. Tick 0
    included so a run shorter than `every_n` ticks (e.g. a short self-test)
    still verifies at least once."""
    return iteration % every_n == 0

ROW_LOCK_TABLE = "_smoke_row_lock_wait"

# Reporter's rotating query shapes (issue #214; advisory-lock holds
# replaced pg_sleep on 2026-10-06 -- see module docstring). Each is a
# DIFFERENT shape (target list / function calls), not just a different
# literal -- pg's query-id jumbling normalizes literals but not shape (see
# Workload.open_extra_session's own docstring), so each gets its own
# query_id, which is what Queries/Matrix/Waterfall/Scatter need to show
# more than one row. Durations span two orders of magnitude on purpose
# (Histogram needs "a spread, not one mode"; Waterfall needs "at least one
# slow enough to be interesting"):
#   catalog lookup      -- sub-ms to a few ms: a real app runs plenty of these
#   cpu aggregate        -- tens to a few hundred ms of genuine CPU work: the
#                           realistic stand-in for a report/aggregation query
#   short advisory hold  -- ~0.4s: reporter's own pg_advisory_lock(42) call
#                           blocks on adv_holder's hold for this long --
#                           Lock:advisory, same class the original loop
#                           guarantees Lock:relation every tick
#   long advisory hold   -- ~1.3s: the "at least one slow enough to be
#                           interesting" execution Waterfall's own acceptance
#                           criterion (issue #214) names explicitly
# None entries are not sent as SQL directly -- _reporter_tick dispatches
# those two slots to _advisory_tick instead (ADVISORY_HOLD_S/
# ADVISORY_TRAILING below carry the per-slot hold duration and the
# distinct trailing columns that keep each shape's own query_id, exactly
# as the two pg_sleep shapes did).
REPORTER_QUERIES = (
    "SELECT count(*) FROM pg_class;",
    "SELECT count(*) FROM generate_series(1, 3000000);",
    None,
    None,
)
# Sleep budget the main loop waits after sending each reporter query, so the
# next tick's SQL is never sent while the previous one is still running.
# Matches REPORTER_QUERIES order; generous over the expected runtime so a
# slower box (a loaded CI runner, an el8/el9 box) still finishes in time.
# Unused for the advisory-hold slots (_advisory_tick manages its own
# timing end to end).
REPORTER_BUDGETS_S = (0.5, 2.0, None, None)

ADVISORY_LOCK_ID = 42
# Per-slot (hold duration, trailing columns) for the advisory-hold ticks;
# aligned with REPORTER_QUERIES' None entries at index 2 and 3.
ADVISORY_HOLD_S = (None, None, 0.4, 1.3)
ADVISORY_TRAILING = (None, None, "1", "2, 3")


def _advisory_tick(adv_holder, reporter, hold_s, trailing_sql):
    """One Lock:advisory cycle: adv_holder takes the pg_advisory_lock(42)
    mutex and holds it for a CLIENT-SIDE `hold_s` (this process sleeping,
    never a server-side pg_sleep), while the reporter's own
    pg_advisory_lock(42) call blocks on the same lock id for that long --
    a real Lock:advisory wait of exactly the hold duration, reading as an
    application mutex rather than a manufactured sleep. `trailing_sql`
    keeps the reporter's two advisory-hold shapes on distinct query_ids
    (pg's query-id jumbling normalizes literals but not shape), exactly as
    the two pg_sleep shapes it replaced did."""
    adv_holder.stdin.write(f"SELECT pg_advisory_lock({ADVISORY_LOCK_ID});\n")
    adv_holder.stdin.flush()
    time.sleep(0.2)   # let adv_holder acquire before the reporter tries
    reporter.stdin.write(
        f"SELECT pg_advisory_lock({ADVISORY_LOCK_ID}), {trailing_sql};\n")
    reporter.stdin.flush()
    time.sleep(hold_s)   # adv_holder holds the mutex this long (client-side)
    adv_holder.stdin.write(
        f"SELECT pg_advisory_unlock({ADVISORY_LOCK_ID});\n")
    adv_holder.stdin.flush()
    time.sleep(0.3)   # let the reporter's blocked statement land
    # The reporter now holds the lock itself (its own pg_advisory_lock call
    # succeeded) -- release it so the next cycle's adv_holder acquire does
    # not block on a leftover hold from this session.
    reporter.stdin.write(f"SELECT pg_advisory_unlock({ADVISORY_LOCK_ID});\n")
    reporter.stdin.flush()
    time.sleep(0.2)


def _reporter_tick(reporter, adv_holder, iteration):
    idx = iteration % len(REPORTER_QUERIES)
    query = REPORTER_QUERIES[idx]
    if query is not None:
        reporter.stdin.write(query + "\n")
        reporter.stdin.flush()
        time.sleep(REPORTER_BUDGETS_S[idx])
    else:
        _advisory_tick(adv_holder, reporter,
                        ADVISORY_HOLD_S[idx], ADVISORY_TRAILING[idx])


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


# pgbench_accounts (scale 10, provisioned by tests/provision-runner.sh) has
# 1,000,000 rows (aid 1..1,000,000) and measures ~187MB with its indexes
# against this box's 128MB shared_buffers (measured live, 2026-10-06).
# 2026-10-07: an earlier version of this tick ran a full `SELECT count(*)`
# sequential scan every tick -- real IO:DataFileRead, but a heavy ~150MB
# scan for no measured benefit (it did not change the Transitions tab's
# node count either way -- see demo_rehearsal_lib's/the box-check report's
# own node-count comparison). Replaced with an indexed single-row read on
# a RANDOM aid: a primary-key index lookup still produces a real
# IO:DataFileRead whenever that row's page is not already in
# shared_buffers (which a random aid across 1M rows against 128MB of
# buffers makes likely most of the time), at a small fraction of the
# scan's cost -- the IO:DataFileRead class the owner asked the IO panels
# to show, without the heavy per-tick scan.
IO_READER_TABLE = "pgbench_accounts"
IO_READER_AID_MAX = 1_000_000


def _io_reader_tick(io_reader):
    """A real IO-bound statement, no sleep: an indexed single-row read on a
    random aid, competing for buffer space against pgbench's own OLTP
    traffic the way a point-lookup reporting query would in production."""
    aid = random.randint(1, IO_READER_AID_MAX)
    io_reader.stdin.write(
        f"SELECT abalance FROM {IO_READER_TABLE} WHERE aid = {aid};\n")
    io_reader.stdin.flush()
    time.sleep(0.2)   # let the lookup land before the next tick's SQL


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

    # Extra persistent sessions (issue #214; adv_holder/io_reader added
    # 2026-10-06), alongside -- not inside -- the holder/waiter/sleeper
    # trio above. open_extra_session() sends its SQL immediately at
    # creation, which is right for a fire-once session but wrong here
    # (reporter/row_holder/row_waiter/adv_holder/io_reader are each sent
    # DIFFERENT SQL on every tick for the whole run), so these are opened
    # directly via
    # Workload's own session-creation contract instead: a plain psql pipe
    # tagged under the same tag_base, reaped by the same stop() call because
    # it matches PGAPPNAME LIKE '{tag_base}%'.
    reporter = wl._session("reporter")
    row_holder = wl._session("row_holder")
    row_waiter = wl._session("row_waiter")
    adv_holder = wl._session("adv_holder")
    io_reader = wl._session("io_reader")
    wl.extra_sessions.extend(
        [reporter, row_holder, row_waiter, adv_holder, io_reader])

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
            # verify=True on most ticks would spawn a fresh one-shot psql
            # backend every ~5-8s for the WHOLE demo window (up to 900s) --
            # the dominant source of the ~150 distinct PIDs polluting the
            # Sessions tab in a demo workload with ~8 real sessions (#243).
            # verify=False every tick removes that churn but also removes
            # the ONLY thing that would ever notice this loop's re-lock
            # silently breaking (#243 review round 2 -- see module
            # docstring), so verify periodically instead and treat a
            # failure as fatal, not print-and-continue.
            verify_this_tick = should_verify_tick(iteration)
            # sleep_s=0: skip the sleeper's pg_sleep statement entirely --
            # the demo's slow query is the real waiter-blocked-on-holder
            # Lock:relation wait, not a manufactured sleep (owner
            # 2026-10-06; see module docstring). The blocking SQL on
            # wl.waiter is unconditional and unaffected.
            ok = wl.fire(sleep_s=0, verify=verify_this_tick)
            if verify_this_tick and not ok:
                print(f"FATAL: live_loop_workload: waiter did not block on "
                      f"tick {iteration} -- the re-lock loop's Lock:relation "
                      f"wait has broken; exiting so the caller's liveness "
                      f"check (_assert_workload_alive) fails the run loudly "
                      f"instead of silently grading a broken workload",
                      file=sys.stderr)
                sys.exit(1)
            _reporter_tick(reporter, adv_holder, iteration)
            _row_lock_tick(row_holder, row_waiter)
            _io_reader_tick(io_reader)
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
