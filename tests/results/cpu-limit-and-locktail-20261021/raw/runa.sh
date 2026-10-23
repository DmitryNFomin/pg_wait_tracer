#!/bin/bash
# runa.sh TAG OBS(pgws|tracer) BATCH(none|batch|chunked) [POLL=1] [DLT=deadlock_timeout]
# 8 request clients at a fixed 600 tx/s (Poisson) + optional batch job (-R 1, Poisson mean 1/s); 60 s window.
set -uo pipefail
TAG=$1; OBS=$2; BATCH=$3; POLL=${POLL:-0}; DLT=${DLT:-}
cd /root/case-ab; mkdir -p a-$TAG; rm -rf a-$TAG/*; cd a-$TAG
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d postgres"
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
if [ -n "$DLT" ]; then $P -c "alter system set deadlock_timeout = '$DLT'" -c "select pg_reload_conf()" >/dev/null; fi
$P -tAc "show deadlock_timeout" > deadlock_timeout
LOGPOS=$(stat -c %s /var/log/postgresql/postgresql-18-main.log)
PGAPPNAME=req pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d postgres -M prepared -c 8 -j 4 -R 600 -T 80 \
  -f ../req.sql -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
PR=""
if [ $BATCH != none ]; then
  F=../batch.sql; [ $BATCH = chunked ] && F=../batch_chunked.sql
  PGAPPNAME=batch pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d postgres -c 1 -T 80 -R 1 -f $F \
    -l --log-prefix=bat > pgb-batch.txt 2>&1 & PR=$!
fi
sleep 10
BP=$($P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='req'")
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null
PP=""
if [ $POLL = 1 ]; then PGAPPNAME=poller timeout 60 psql -X -qAt -F' ' -h /var/run/postgresql -p 5418 -U postgres -d postgres -f ../poll.sql > blockers.txt 2>&1 & PP=$!; fi
date +%s.%N > t0
if [ $OBS = tracer ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 60 --view system_event -T trace > tracer-system_event.txt 2>&1
else sleep 60; fi
date +%s.%N > t1
$P -tAc "copy (select coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         where pid in ($BP) group by 1,2 order by 3 desc) to stdout with csv" > pgws.csv
$P -tAc "copy (select left(regexp_replace(s.query,'\s+',' ','g'),60), coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
         from pg_wait_sampling_profile p left join pg_stat_statements s on s.queryid = p.queryid
         where p.pid in ($BP) group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-query.csv
$P -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),60), calls, round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2), queryid
         from pg_stat_statements where query ilike any (array['UPDATE pgbench_accounts%','SELECT sum(abalance)%']) order by 2 desc) to stdout with csv" > pgss.csv
echo $BP > req-pids
$P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='batch'" > batch-pids
wait $PB $PR $PP 2>/dev/null
tail -c +$((LOGPOS+1)) /var/log/postgresql/postgresql-18-main.log > pglog.txt
if [ -n "$DLT" ]; then $P -c "alter system reset deadlock_timeout" -c "select pg_reload_conf()" >/dev/null; fi
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*; cat bat.* > bat-all.txt 2>/dev/null; rm -f bat.*
echo "$TAG done: $(grep -E '^tps' pgb.txt) | log 'still waiting' lines: $(grep -c 'still waiting for' pglog.txt)"
