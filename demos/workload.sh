#!/usr/bin/env bash
# workload.sh — Drives a 60-second mixed-wait workload on PG.
#
# Runs ON the DB server (or wherever PG is reachable). Generates a varied
# wait-event mix: pgbench TPC-B (Lock, WAL, Client), injected LOCK TABLE
# (Lock:relation + a genuinely slow statement, see below), and a few large
# reads (CPU + buffer activity).
#
# Used by demos/capture-trace.sh after the tracer is started.
#
# Issue #242: Waterfall's 500ms coverage floor never saw a real execution
# above it on this workload, so the demo's Waterfall tab was blank. The
# fix is NOT a floor change (src/ is out of scope and the floor is doing
# its job) and NOT pg_sleep (ruled out deliberately: a sleeping backend is
# not a waiting one, and it does not produce the wait profile a real slow
# statement does -- see the lock-burst comment below for the measured
# replacement). The lock-burst's holder now runs a real external-merge
# sort instead of pg_sleep(2), which is both genuinely slow itself AND
# blocks every pgbench TPC-B transaction (which all UPDATE
# pgbench_branches) on Lock:relation for comparable wall time.
#
# Env:
#   PSQL    — psql binary (default: psql)
#   PGBENCH — pgbench binary (default: pgbench)
#   DB      — target database (default: postgres)
#   PGUSER  — DB role (default: postgres)
#   CLIENTS — pgbench concurrency (default: 8)
#   DURATION_SEC — pgbench duration (default: 60)

set -euo pipefail

PSQL=${PSQL:-psql}
PGBENCH=${PGBENCH:-pgbench}
DB=${DB:-postgres}
PGUSER=${PGUSER:-postgres}
CLIENTS=${CLIENTS:-8}
DURATION_SEC=${DURATION_SEC:-60}

echo "=== Warmup ==="
$PSQL -U "$PGUSER" -d "$DB" -v ON_ERROR_STOP=1 -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm; SELECT pg_prewarm('pgbench_accounts');"

echo "=== Starting mixed workload (${DURATION_SEC}s, ${CLIENTS} clients) ==="

# Background: 5 transient LOCK TABLE bursts to seed Lock:relation waits AND
# give Waterfall something above its 500ms floor (issue #242). The holder
# takes an EXCLUSIVE lock on pgbench_branches (every pgbench TPC-B
# transaction UPDATEs it, so this throttles the whole fleet) and, instead of
# pg_sleep, runs a real external-merge sort over pgbench_accounts's
# 1,000,000 rows -- forced to spill to disk by a reduced work_mem, with
# parallel workers off so the whole sort runs (and is timed) as one backend.
# Measured locally against a scale-10 pgbench_accounts (PostgreSQL 13,
# default 128MB shared_buffers -- same default the demo VM's cloud-init
# leaves in place): three runs of the bare query took 993/1046/1103 ms, and
# EXPLAIN (ANALYZE, BUFFERS) confirmed "Sort Method: external merge" with
# real temp-file read/write, not a cache hit. With the lock held, a
# concurrent UPDATE on pgbench_branches measured 674.8 ms blocked
# (Lock:relation) before the holder's commit released it. So this one
# statement produces two DISTINCT kinds of genuinely slow completed
# execution -- the holder's own (Sort + I/O) and each blocked waiter's
# (Lock:relation, spread across the hold window depending on arrival order)
# -- comfortably above the 500ms floor from real work, not a manufactured
# delay.
(
  lock_pids=()
  for _ in 1 2 3 4 5; do
    sleep 8
    $PSQL -U "$PGUSER" -d "$DB" -v ON_ERROR_STOP=1 \
      -c "BEGIN; LOCK TABLE pgbench_branches IN EXCLUSIVE MODE; SET work_mem = '1MB'; SET max_parallel_workers_per_gather = 0; SELECT count(*) FROM (SELECT aid, bid, abalance, filler FROM pgbench_accounts ORDER BY filler, abalance) t; COMMIT;" \
      >/dev/null &
    lock_pids+=("$!")
  done
  for lock_pid in "${lock_pids[@]}"; do wait "$lock_pid"; done
) &
LOCK_PID=$!

# Background: heavy reads to surface IO + buffer activity
(
  for _ in 1 2 3; do
    $PSQL -U "$PGUSER" -d "$DB" -v ON_ERROR_STOP=1 \
      -c "SELECT count(*) FROM pgbench_accounts WHERE aid % 7 = 0;" \
      >/dev/null
  done
) &
READ_PID=$!

# Foreground: pgbench TPC-B
$PGBENCH -U "$PGUSER" -c "$CLIENTS" -j 2 -T "$DURATION_SEC" -P 10 -r "$DB" 2>&1

wait "$LOCK_PID"
wait "$READ_PID"
echo "=== Workload done ==="
