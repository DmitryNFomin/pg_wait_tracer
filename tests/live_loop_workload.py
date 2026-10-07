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

  - `reporter`: rotates through six structurally distinct, realistic
    SELECTs every tick (a catalog lookup, a CPU-bound aggregate, and FOUR
    differently-shaped advisory-lock holds -- see `adv_holder` below;
    expanded from two to four 2026-10-07, owner scope addition, "add more
    load") -- gives Queries/Histogram/Waterfall/Scatter more than one
    query_id and a real spread of durations instead of the single
    Lock:relation wait the original loop produces alone.
  - `adv_holder`: a persistent session that holds `pg_advisory_lock(id)`
    for a CLIENT-SIDE `time.sleep()` (never a server-side `pg_sleep`,
    never `generate_series` -- its timing is CPU-dependent and the gate
    boxes have different silicon) while the reporter's own
    `pg_advisory_lock(id)` call blocks on it -> `Lock:advisory` of exactly
    the hold duration, reading as an application mutex rather than a
    manufactured sleep (owner 2026-10-06: a `pg_sleep(1.3)` visible in the
    Top Queries panel reads as a faked demo to any DBA in the room). Four
    distinct lock ids and a 0.1/0.3/1.0/3.0s hold spread since 2026-10-07
    (dropping pg_sleep cost the demo ~36s of visible DB Time per 90s
    window and the original two-shape replacement only gave back ~3.4s --
    see ADVISORY_HOLD_S's own comment).
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
    there. Also runs `_io_load_read_tick` (below) every tick.
  - `io_writer`: added 2026-10-07 (owner scope addition, "add more load" --
    IO:DataFileRead/Write explicitly requested, still ~0.1% with just
    `io_reader`'s one point-read per tick). Runs `_io_load_write_tick`
    (below) every tick: a BATCH of scattered single-row UPDATEs against
    IO_LOAD_TABLE, a dedicated side table sized above shared_buffers
    (provisioned once by tests/provision-runner.sh, not created here --
    see that script's own comment for the measured build time/disk cost).
    Dirties pages that the background writer/checkpointer later evict,
    producing real `IO:DataFileWrite`. No sleep anywhere in the batch.
  - `lockmgr_0`..`lockmgr_7`: added 2026-10-07 (same scope addition;
    `LWLock:LockManager` explicitly requested weeks ago, still ~0.0%).
    Eight CONCURRENT sessions fire the same query against a 200-partition
    table every tick, filtered on a non-partition-key column so no
    partition can be pruned -- a single backend's fast path holds at most
    16 weak relation locks (a fixed PostgreSQL constant), so touching 200
    relations in one query falls through to the shared, partitioned lock
    manager that `LWLock:LockManager` guards. The table and its partitions
    are intentionally empty (a lock is taken on every unpruned partition
    at plan/open time regardless of row count) -- this isolates lock
    acquisition cost from `io_reader`/`io_writer`'s I/O cost. No sleep.

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
#   catalog lookup   -- sub-ms to a few ms: a real app runs plenty of these
#   cpu aggregate     -- tens to a few hundred ms of genuine CPU work: the
#                        realistic stand-in for a report/aggregation query
#   four advisory holds -- 0.1/0.3/1.0/3.0s (owner 2026-10-07: dropping
#                        pg_sleep cost the demo ~36s of visible DB Time per
#                        90s window and the original two-shape replacement
#                        only gave back ~3.4s; richer duration spread was
#                        explicitly requested over schedule safety). Each
#                        is its own lock id (a realistic app has more than
#                        one named mutex) AND its own trailing-column count
#                        (what actually drives a distinct query_id -- see
#                        _advisory_tick's own docstring)
# None entries are not sent as SQL directly -- _reporter_tick dispatches
# those slots to _advisory_tick instead (ADVISORY_LOCK_IDS/ADVISORY_HOLD_S/
# ADVISORY_TRAILING below carry each slot's lock id, hold duration and
# distinct trailing columns).
REPORTER_QUERIES = (
    "SELECT count(*) FROM pg_class;",
    "SELECT count(*) FROM generate_series(1, 3000000);",
    None,
    None,
    None,
    None,
)
# Sleep budget the main loop waits after sending each reporter query, so the
# next tick's SQL is never sent while the previous one is still running.
# Matches REPORTER_QUERIES order; generous over the expected runtime so a
# slower box (a loaded CI runner, an el8/el9 box) still finishes in time.
# Unused for the advisory-hold slots (_advisory_tick manages its own
# timing end to end).
REPORTER_BUDGETS_S = (0.5, 2.0, None, None, None, None)

# Per-slot (lock id, hold duration, trailing columns) for the advisory-hold
# ticks; aligned with REPORTER_QUERIES' None entries at index 2-5. Distinct
# lock ids (2026-10-07: "several distinct lock ids", owner) so this reads
# as several independent named mutexes, not one lock reused four ways;
# distinct trailing-column COUNTS (not just distinct literals -- jumbling
# normalizes those away) so each of the four keeps its own query_id.
ADVISORY_LOCK_IDS = (None, None, 42, 43, 44, 45)
ADVISORY_HOLD_S = (None, None, 0.1, 0.3, 1.0, 3.0)
ADVISORY_TRAILING = (None, None, "1", "2, 3", "4, 5, 6", "7, 8, 9, 10")


def _advisory_tick(adv_holder, reporter, lock_id, hold_s, trailing_sql):
    """One Lock:advisory cycle: adv_holder takes the pg_advisory_lock(lock_id)
    mutex and holds it for a CLIENT-SIDE `hold_s` (this process sleeping,
    never a server-side pg_sleep), while the reporter's own
    pg_advisory_lock(lock_id) call blocks on the same lock id for that
    long -- a real Lock:advisory wait of exactly the hold duration, reading
    as an application mutex rather than a manufactured sleep. `trailing_sql`
    keeps each of the four advisory-hold shapes on its own distinct
    query_id (pg's query-id jumbling normalizes literals -- including the
    lock id itself -- but not shape, i.e. trailing-column COUNT), the same
    way the two pg_sleep shapes this mechanism replaced did."""
    adv_holder.stdin.write(f"SELECT pg_advisory_lock({lock_id});\n")
    adv_holder.stdin.flush()
    time.sleep(0.2)   # let adv_holder acquire before the reporter tries
    reporter.stdin.write(
        f"SELECT pg_advisory_lock({lock_id}), {trailing_sql};\n")
    reporter.stdin.flush()
    time.sleep(hold_s)   # adv_holder holds the mutex this long (client-side)
    adv_holder.stdin.write(f"SELECT pg_advisory_unlock({lock_id});\n")
    adv_holder.stdin.flush()
    time.sleep(0.3)   # let the reporter's blocked statement land
    # The reporter now holds the lock itself (its own pg_advisory_lock call
    # succeeded) -- release it so the next cycle's adv_holder acquire does
    # not block on a leftover hold from this session.
    reporter.stdin.write(f"SELECT pg_advisory_unlock({lock_id});\n")
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
        _advisory_tick(adv_holder, reporter, ADVISORY_LOCK_IDS[idx],
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


# 2026-10-07 owner scope addition ("add more load - we need load observed
# machine otherwise what we will be looking at?"): a single point read per
# ~8-10s tick measured at ~0.1% of DB Time -- not visible on the IO panels.
# IO_LOAD_TABLE is a dedicated side table (provisioned once by
# tests/provision-runner.sh, NOT created here -- a 500MB-1GB table is too
# expensive to build inline on every run; see provision-runner.sh's own
# comment for the measured build time/disk cost) sized comfortably above
# this box's 128MB shared_buffers, so random single-row reads keep missing
# the buffer cache (real IO:DataFileRead) and scattered single-row UPDATEs
# keep dirtying pages that get evicted/written back (real
# IO:DataFileWrite) -- "constant read misses" and "dirty-buffer eviction
# writes" per the owner's own framing. Driven as BATCHES of statements per
# tick (not one statement per ~9s tick) so the volume is actually visible
# against the window's dominant Lock:relation/CPU time, still with no
# server-side sleep anywhere -- batching is pure statement count, not
# timing.
IO_LOAD_TABLE = "_smoke_io_load"
IO_LOAD_ROWS = 3_000_000
# Measured live (gate-2, 2026-10-07): 200 reads / 100 writes per tick gave
# IO:DataFileRead 0.2% / IO:DataFileWrite ~0.0% of DB Time over a 120s
# capture -- present but easy to miss. 600/400 measured 0.4% / 0.1% on the
# same capture shape. Lock:relation dominates this workload's DB Time
# (~49%) regardless, so these percentages were never going to rival it --
# reported honestly rather than inflated further by guessing at a much
# larger batch.
IO_LOAD_READS_PER_TICK = 600
IO_LOAD_WRITES_PER_TICK = 400


def _io_load_read_tick(io_reader):
    """IO_LOAD_READS_PER_TICK random single-row reads by primary key,
    batched into one write() so they queue on the session without a
    Python-level round trip per statement -- real IO:DataFileRead on
    whichever ones miss shared_buffers, no sleep anywhere in the batch."""
    stmts = "".join(
        f"SELECT filler FROM {IO_LOAD_TABLE} WHERE id = "
        f"{random.randint(1, IO_LOAD_ROWS)};\n"
        for _ in range(IO_LOAD_READS_PER_TICK))
    io_reader.stdin.write(stmts)
    io_reader.stdin.flush()
    time.sleep(0.3)   # let the batch mostly land before the next tick's SQL


def _io_load_write_tick(io_writer):
    """IO_LOAD_WRITES_PER_TICK scattered single-row UPDATEs by primary key
    -- dirties pages that the background writer/checkpointer later evict,
    producing real IO:DataFileWrite. Same batching rationale as the read
    side; no sleep anywhere in the batch."""
    stmts = "".join(
        f"UPDATE {IO_LOAD_TABLE} SET ctr = ctr + 1 WHERE id = "
        f"{random.randint(1, IO_LOAD_ROWS)};\n"
        for _ in range(IO_LOAD_WRITES_PER_TICK))
    io_writer.stdin.write(stmts)
    io_writer.stdin.flush()
    time.sleep(0.3)   # let the batch mostly land before the next tick's SQL


# 2026-10-07 owner scope addition: LWLock:LockManager, requested explicitly
# weeks ago, still ~0.0%. Mechanism (owner's own): a single backend's FAST
# PATH holds at most 16 weak relation locks (a fixed PostgreSQL constant,
# not a GUC) -- a transaction that touches MORE than 16 distinct relations
# falls through to the shared lock manager's partitioned hash table for the
# overflow, which is what LWLock:LockManager actually guards. LOCKMGR_TABLE
# is provisioned once by tests/provision-runner.sh as a RANGE-partitioned
# table with LOCKMGR_PARTITIONS child partitions (empty -- no data needed:
# a lock is taken on every unpruned partition at plan/open time regardless
# of row count). LOCKMGR_QUERY filters on a non-partition-key column, so no
# partition can be pruned and every query opens (and locks) all of them --
# "a predicate that defeats partition pruning" per the owner's framing.
# Several CONCURRENT sessions (not just one) fire this at roughly the same
# moment so the shared lock table partitions actually contend.
LOCKMGR_TABLE = "_smoke_lockmgr_fanout"
LOCKMGR_PARTITIONS = 200
LOCKMGR_SESSION_COUNT = 8
LOCKMGR_QUERIES_PER_TICK = 20
# val is not the partition key (id is) -- this is the predicate that
# defeats pruning. The table and all its partitions are empty by design
# (see module comment); this is a pure lock-acquisition cost, not an I/O
# one, which is the point -- it isolates LWLock:LockManager from
# IO_LOAD_TABLE's own IO:DataFileRead/Write above.
LOCKMGR_QUERY = f"SELECT count(*) FROM {LOCKMGR_TABLE} WHERE val > 0;\n"


def _lockmgr_tick(lockmgr_sessions):
    """Fires LOCKMGR_QUERIES_PER_TICK repetitions of the pruning-defeating
    fanout query to every lockmgr session with minimal gap between writes,
    so the sessions' executions genuinely overlap at the server and
    contend for the shared lock manager's partition locks. No sleep
    anywhere in the batch -- the ONLY timing is the 0.3s landing budget
    after every session has been sent its batch."""
    stmts = LOCKMGR_QUERY * LOCKMGR_QUERIES_PER_TICK
    for sess in lockmgr_sessions:
        sess.stdin.write(stmts)
        sess.stdin.flush()
    time.sleep(0.3)   # let the concurrent batches mostly land


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
    io_writer = wl._session("io_writer")
    lockmgr_sessions = [wl._session(f"lockmgr_{i}")
                         for i in range(LOCKMGR_SESSION_COUNT)]
    wl.extra_sessions.extend(
        [reporter, row_holder, row_waiter, adv_holder, io_reader,
         io_writer, *lockmgr_sessions])

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
            _io_load_read_tick(io_reader)
            _io_load_write_tick(io_writer)
            _lockmgr_tick(lockmgr_sessions)
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
