#!/bin/bash
# c3_measure.sh TAG: 60 s of PK lookups (ids 1..200000, 8 clients) observed by pg_wait_sampling, pg_stat_statements,
# pg_stat_io, and pg_wait_tracer --mode full (trace dir kept for offline views). Kernel/fio context is set by the caller.
set -uo pipefail
TAG=$1; cd /root/case-cl
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d case3"
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
mkdir -p c3-$TAG; rm -rf c3-$TAG/*; cd c3-$TAG
cat > lookup.sql <<SQL
\set id random(1, ${RANGE:-200000})
SELECT filler FROM t WHERE id = :id;
SQL
pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d case3 -M prepared -f lookup.sql -c 8 -j 4 -T 80 -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
sleep 10
BP=$($P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='pgbench'")
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null; $P -tAc "select pg_stat_reset_shared('io')" >/dev/null
date +%s.%N > t0
/root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 60 --view system_event -T trace > tracer-system_event.txt 2>&1
date +%s.%N > t1
$P -tAc "copy (select coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         where pid in ($BP) group by 1,2 order by 3 desc) to stdout with csv" > pgws.csv
$P -tAc "copy (select calls, shared_blks_hit, shared_blks_read, round(shared_blk_read_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,1)
         from pg_stat_statements where query like 'SELECT filler FROM t WHERE id%') to stdout with csv" > pgss.csv
$P -tAc "copy (select reads, round(read_time::numeric,1) from pg_stat_io where object='relation' and backend_type='client backend' and reads>0) to stdout with csv" > pgio.csv
wait $PB
/root/pg_wait_tracer --replay -T trace --view histogram -e DataFileRead > tracer-hist.txt 2>&1
/root/pg_wait_tracer --replay -T trace --view query_event > tracer-query.txt 2>&1
cat lat.* | awk '{print $3}' | sort -n > lat_us.txt; rm -f lat.*
echo "$TAG done: $(grep -E '^tps' pgb.txt)"
