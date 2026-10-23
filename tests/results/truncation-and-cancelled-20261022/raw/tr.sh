#!/bin/bash
# tr.sh TAG OBS(pgws|full|poll) TRUNC(on|off): autovacuum truncation convoy. 300 s, DELETE trailing half at 60 s.
set -uo pipefail
TAG=$1; OBS=$2; TRUNC=$3; DUR=${DUR:-300}
cd /root/t; mkdir -p tr-$TAG; rm -rf tr-$TAG/*; cd tr-$TAG
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres"
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
$P -d postgres -c "drop database if exists tr" -c "create database tr template trsrc strategy file_copy" >/dev/null
$P -d tr -c "alter table t set (autovacuum_enabled = true, vacuum_truncate = $TRUNC)" -c "select pg_prewarm('t'), pg_prewarm('t_pkey')" -tA > prewarm.txt
$P -d tr -c "create extension if not exists pg_stat_statements" >/dev/null 2>&1
LOGPOS=$(stat -c %s /var/log/postgresql/postgresql-18-main.log)
$P -d postgres -tAc "select pg_wait_sampling_reset_profile(); select pg_stat_statements_reset()" >/dev/null
date +%s.%N > t0
PGAPPNAME=req pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d tr -M ${PROTO:-prepared} -c 16 -j 4 -R 8000 -T $DUR \
  -f ../lookup.sql -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
PGAPPNAME=progress timeout $DUR psql -X -qAt -F' ' -h /var/run/postgresql -p 5418 -U postgres -d tr -f ../progress.sql > progress.txt 2>&1 &
PP=""
if [ $OBS = poll ]; then PGAPPNAME=poller timeout $DUR psql -X -qAt -F' ' -h /var/run/postgresql -p 5418 -U postgres -d tr -f ../poll.sql > blockers.txt 2>&1 & PP=$!; fi
if [ $OBS = full ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval $((DUR-5)) --view system_event -T trace > tracer-system_event.txt 2>&1 & PT=$!
fi
sleep 60
( date +%s.%N; $P -d tr -c "\timing on" -c "delete from t where id > 10000000"; date +%s.%N ) > delete.txt 2>&1
wait $PB
[ $OBS = full ] && wait $PT
date +%s.%N > t1
$P -d tr -tAc "copy (select pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-all.csv
$P -d tr -tAc "copy (select left(regexp_replace(s.query,'\s+',' ','g'),60), coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
   from pg_wait_sampling_profile p left join pg_stat_statements s on s.queryid = p.queryid group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-query.csv
$P -d tr -tAc "copy (select ts, pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), queryid from pg_wait_sampling_history) to stdout with csv" > pgws-history.csv
$P -d tr -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),60), calls, round(total_exec_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2), queryid from pg_stat_statements order by 3 desc limit 10) to stdout with csv" > pgss.csv
$P -d tr -tAc "select pg_size_pretty(pg_relation_size('t'))" > size-after.txt
tail -c +$((LOGPOS+1)) /var/log/postgresql/postgresql-18-main.log > pglog.txt
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*
if [ $OBS = full ]; then
  /root/pg_wait_tracer --replay -T trace --view histogram -e Lock:relation > tracer-hist-lockrel.txt 2>&1
  /root/pg_wait_tracer --replay -T trace --view query_event > tracer-query.txt 2>&1
fi
echo "$TAG done: $(grep -E '^tps' pgb.txt) | size after $(cat size-after.txt) | still-waiting lines $(grep -c 'still waiting' pglog.txt) | truncate lines $(grep -c -i 'truncat\|pages: [0-9]* removed' pglog.txt)"
