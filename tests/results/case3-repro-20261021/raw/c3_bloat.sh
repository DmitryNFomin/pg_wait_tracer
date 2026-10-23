#!/bin/bash
# Build the "bloat" condition: an idle-in-transaction snapshot plus 30 rounds of updates on ids 1..200000.
set -uo pipefail
cd /root/case-cl
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d case3"
( echo "SET application_name='reporting_job'; BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t WHERE id = 1;"; sleep 3000 ) | $P > holder.out 2>&1 &
echo $! > holder.pid
sleep 2
$P -tAc "select pid, state, backend_xmin from pg_stat_activity where application_name='reporting_job'"
for i in $(seq 30); do $P -tAc "update t set grp = grp + 1 where id <= 200000" >/dev/null; done
$P -tAc "select n_live_tup, n_dead_tup, pg_size_pretty(pg_table_size('t')), pg_size_pretty(pg_indexes_size('t')) from pg_stat_user_tables where relname='t'"
