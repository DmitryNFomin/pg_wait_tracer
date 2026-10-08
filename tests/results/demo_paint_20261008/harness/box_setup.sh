#!/usr/bin/env bash
# Runs ON gate-1 (2026-10-08 re-measure). Takes the box-check flock for the whole measurement session,
# builds the tree that was rsynced in, and starts the demo workload
# (pgbench -c4 --rate=25 + tests/live_loop_workload.py + tests/burst_load.sh
# -- exactly what tests/demo_rehearsal.sh / tests/ui_live_smoke.sh start as of
# 2026-10-08) for LONG enough to cover a fresh 900 s capture.
#
# The BURST tier is the point of this re-measure: it is new load, and the
# risk it was flagged for is exactly that its extra event volume pushes tab
# paint past the 3000 ms bound. A paint run without it would measure the
# wrong system and report a reassuring number.
#
# It does NOT start the daemon: each of the four runs gets its own EMPTY trace
# dir (PR #295 refuses pre-v4 summaries and would silently fall back to the
# slow raw path on a reused dir), started by box_run_start.sh.
set -uo pipefail
ROOT=/root/pgwt-paint-20261008
cd "$ROOT" || exit 2

# Hold the lock for the whole session in a detached process, so an in-flight
# CI job or another agent's box-check cannot interleave with the measurement.
# flock -n: never queue silently -- if the box is busy we must know now.
setsid nohup flock -n /tmp/pgwt-box-check.lock \
    bash -c 'echo held; sleep 2700' > /tmp/paint1_lock.log 2>&1 < /dev/null &
sleep 2
if ! grep -q held /tmp/paint1_lock.log; then
    echo "LOCK BUSY -- refusing to measure on a contended box"; exit 1
fi
echo "lock: held (pid $(pgrep -f 'sleep 2700' | head -1))"

echo "=== build ==="
make -j4 2>&1 | tail -3
for b in pg_wait_tracer pgwt-server; do
    [[ -x "$ROOT/$b" ]] || { echo "MISSING BINARY $b"; exit 1; }
done
echo "binaries: $(ls -la "$ROOT"/pg_wait_tracer "$ROOT"/pgwt-server | awk '{print $NF, $5}' | tr '\n' ' ')"

echo "=== fixtures (must already be provisioned; this script never builds them) ==="
for q in "SELECT count(*) FROM _smoke_io_load" \
         "SELECT pg_size_pretty(pg_total_relation_size('_smoke_io_load'))" \
         "SELECT count(*) FROM pg_inherits WHERE inhparent = '_smoke_lockmgr_fanout'::regclass" \
         "SELECT count(*) FROM pgbench_accounts" \
         "SHOW shared_buffers"; do
    printf '  %-80s = %s\n' "$q" "$(sudo -u postgres psql -p 5418 -tAc "$q" 2>&1 | tr -d '\n')"
done

echo "=== workload ==="
PGBENCH_DURATION=${PGBENCH_DURATION:-2400}
setsid nohup pgbench -U postgres -p 5418 -d postgres -c 4 -T "$PGBENCH_DURATION" --rate=25 \
    > /tmp/paint1_pgbench.log 2>&1 < /dev/null &
sleep 3
PGBENCH_PID=$(pgrep -f "pgbench -U postgres -p 5418" | head -1)
setsid nohup python3 tests/live_loop_workload.py "$PGBENCH_DURATION" \
    > /tmp/paint1_workload.log 2>&1 < /dev/null &
sleep 20
WORKLOAD_PID=$(pgrep -f "live_loop_workload.py" | head -1)
# Burst tier. PGPORT must be exported for it (it refuses to derive its own --
# see tests/burst_load.sh's header); 5418 is PG18's port on this box and is
# what every other command in this script already uses.
export PGPORT=5418
setsid nohup env PGWT_BURST_LOG=/tmp/paint1_burst_detail.log \
    bash tests/burst_load.sh "$PGBENCH_DURATION" \
    > /tmp/paint1_burst.log 2>&1 < /dev/null &
sleep 10
BURST_PID=$(pgrep -f "bash tests/burst_load.sh" | head -1)
echo "pgbench pid=$PGBENCH_PID  workload pid=$WORKLOAD_PID  burst pid=$BURST_PID"
[[ -n "$PGBENCH_PID" && -n "$WORKLOAD_PID" && -n "$BURST_PID" ]] || { echo "WORKLOAD FAILED TO START"; tail -20 /tmp/paint1_pgbench.log /tmp/paint1_workload.log /tmp/paint1_burst.log /tmp/paint1_burst_detail.log; exit 1; }
printf 'PGBENCH_PID=%s\nWORKLOAD_PID=%s\nBURST_PID=%s\n' "$PGBENCH_PID" "$WORKLOAD_PID" "$BURST_PID" > /tmp/paint1_pids.env

echo "=== sessions visible to PostgreSQL ==="
sudo -u postgres psql -p 5418 -tAc \
  "SELECT count(*), count(*) FILTER (WHERE application_name LIKE 'pgwt_%') FROM pg_stat_activity WHERE backend_type='client backend'"
echo "=== workload log head ==="
tail -8 /tmp/paint1_workload.log
echo "=== burst log head ==="
tail -12 /tmp/paint1_burst_detail.log
echo "SETUP OK"
