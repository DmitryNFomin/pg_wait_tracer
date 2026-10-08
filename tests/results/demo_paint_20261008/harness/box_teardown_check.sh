#!/usr/bin/env bash
set -uo pipefail
echo "--- client backends left ---"
sudo -u postgres psql -p 5418 -tAc \
  "SELECT count(*) FILTER (WHERE application_name LIKE 'pgwt_%') AS pgwt_sessions,
          count(*) AS client_backends
   FROM pg_stat_activity WHERE backend_type = 'client backend'"
echo "--- workload leftover tables (should be none) ---"
sudo -u postgres psql -p 5418 -tAc \
  "SELECT coalesce(string_agg(relname, ', '), '(none)') FROM pg_class
   WHERE relname IN ('_smoke_row_lock_wait', '_smoke_lock_wait')"
echo "--- fixtures still intact (must be, they are provisioned state) ---"
sudo -u postgres psql -p 5418 -tAc "SELECT count(*) FROM _smoke_io_load"
sudo -u postgres psql -p 5418 -tAc \
  "SELECT count(*) FROM pg_inherits WHERE inhparent = '_smoke_lockmgr_fanout'::regclass"
echo "--- daemons / pgbench / workload ---"
ps -eo args= | grep -E "pg_wait_tracer --daemon|pgbench -U postgres|live_loop_workload" | grep -v grep || echo "(none)"
echo "--- lock ---"
exec 9>/tmp/pgwt-box-check.lock
if flock -n 9; then echo "LOCK FREE"; flock -u 9; else echo "LOCK HELD"; fi
echo "--- load ---"; cat /proc/loadavg
