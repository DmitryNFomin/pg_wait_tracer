#!/bin/bash
# sh.sh TAG SERVER(18|20) OBS(pgws|tracer|patch) JOB(full|chunked) : checkout at 2000/s + restock job ~5/s; 120 s window
set -uo pipefail
TAG=$1; SRV=$2; OBS=$3; JOB=$4
if [ $SRV = 18 ]; then PORT=5418; B=/usr/bin; PMF=/var/lib/postgresql/18/main/postmaster.pid
else PORT=5419; B=/opt/pgwet/bin; PMF=/var/lib/postgresql/pgwet/postmaster.pid; fi
cd /root/t; mkdir -p sh-$TAG; rm -rf sh-$TAG/*; cd sh-$TAG
P="$B/psql -X -q -h /var/run/postgresql -p $PORT -U postgres -d shop"
PM=$(head -1 $PMF)
$P -tAc "show shared_buffers" > shared_buffers.txt; cat /proc/$PM/cgroup > cgroup.txt
python3 -c "import time; print(time.time(), time.clock_gettime(time.CLOCK_MONOTONIC))" > clock0
JF=../restock.sql; [ $JOB = chunked ] && JF=../restock_chunked.sql
PGAPPNAME=req $B/pgbench -n -h /var/run/postgresql -p $PORT -U postgres -d shop -M prepared -c 16 -j 4 -R 2000 -T 150 \
  -f ../checkout.sql -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
PGAPPNAME=restock $B/pgbench -n -h /var/run/postgresql -p $PORT -U postgres -d shop -c 1 -R 5 -T 150 -f $JF -l --log-prefix=job > pgb-job.txt 2>&1 & PJ=$!
sleep 15
$P -tAc "select pg_wait_sampling_reset_profile(); select pg_stat_statements_reset()" >/dev/null
date +%s.%N > t0
if [ $OBS = tracer ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 120 --view system_event -T trace > tracer-system_event.txt 2>&1
else sleep 120; fi
date +%s.%N > t1
if [ $OBS = patch ]; then
  $P -tAc "copy (select a.pid, a.application_name, w.procnumber from pg_stat_activity a join (select distinct pid, procnumber from pg_stat_get_wait_event_timing()) w using (pid) where a.application_name in ('req','restock')) to stdout with csv" > procs.csv
  : > rings.csv
  while IFS=, read pid app pn; do $P -tAc "copy (select $pid, '$app', * from pg_get_wait_event_trace($pn)) to stdout with csv" >> rings.csv; done < procs.csv
fi
$P -tAc "copy (select left(regexp_replace(s.query,'\s+',' ','g'),50), coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
   from pg_wait_sampling_profile p join pg_stat_activity a on a.pid = p.pid and a.application_name = 'req'
   left join pg_stat_statements s on s.queryid = p.queryid group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-query.csv
$P -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),50), calls, round(total_exec_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2), shared_blks_read from pg_stat_statements order by 3 desc limit 8) to stdout with csv" > pgss.csv
wait $PB $PJ
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*; cat job.* > job-all.txt 2>/dev/null; rm -f job.*
[ $OBS = tracer ] && /root/dump_raw trace > raw.txt 2>/dev/null
echo "$TAG done: $(grep -E '^tps' pgb.txt) | job $(grep -E '^tps' pgb-job.txt)"
