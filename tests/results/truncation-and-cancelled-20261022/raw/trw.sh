#!/bin/bash
# trw.sh TAG CAPTURE(off|stats|trace) TRUNC(on|off): truncation convoy on the patched build (port 5419).
# Same workload as tr.sh. With CAPTURE=trace, dumps every lookup backend's wait-event ring and
# pg_wait_event_trace_by_statement() ~3 s after the truncation phase ends.
set -uo pipefail
TAG=$1; CAP=$2; TRUNC=$3; DUR=${DUR:-300}
B=/opt/pgwet/bin
cd /root/t; mkdir -p tw-$TAG; rm -rf tw-$TAG/*; cd tw-$TAG
P="$B/psql -X -q -h /var/run/postgresql -p 5419 -U postgres"
$P -d postgres -c "drop database if exists tr" -c "create database tr template trsrc strategy file_copy" >/dev/null
$P -d postgres -c "alter database tr set pg_wait_event_tracing.capture = '$CAP'" >/dev/null
$P -d tr -c "alter table t set (autovacuum_enabled = true, vacuum_truncate = $TRUNC)" -tA -c "select pg_prewarm('t'), pg_prewarm('t_pkey')" > prewarm.txt
LOGPOS=$(stat -c %s /var/lib/postgresql/pgwet.log)
$P -d tr -tAc "select pg_wait_sampling_reset_profile(); select pg_stat_statements_reset()" >/dev/null
date +%s.%N > t0
PGAPPNAME=req $B/pgbench -n -h /var/run/postgresql -p 5419 -U postgres -d tr -M prepared -c 16 -j 4 -R 8000 -T $DUR \
  -f ../lookup.sql -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
PGAPPNAME=progress timeout $DUR $B/psql -X -qAt -F' ' -h /var/run/postgresql -p 5419 -U postgres -d postgres -f ../progress.sql > progress.txt 2>&1 &
sleep 60
( date +%s.%N; $P -d tr -c "\timing on" -c "delete from t where id > 10000000"; date +%s.%N ) > delete.txt 2>&1
if [ $CAP = trace ]; then
  # wait for the truncation phase to start and end
  for i in $(seq 600); do grep -q "truncating heap" progress.txt && break; sleep 0.5; done
  for i in $(seq 600); do
    last=$(grep truncating progress.txt | tail -1 | cut -d' ' -f1); now=$(date +%s.%N)
    awk -v n=$now -v l=$last 'BEGIN{exit !(n-l>1.5)}' && break; sleep 0.5
  done
  sleep 3; date +%s.%N > dump_t
  $P -d tr -tAc "copy (select pid, procnumber from pg_stat_activity a, lateral (select procnumber from pg_stat_get_wait_event_timing() w where w.pid = a.pid limit 1) x where a.application_name = 'req') to stdout with csv" > req-procs.csv
  : > rings.csv; : > by_statement.csv
  while IFS=, read pid pn; do
    $P -d tr -tAc "copy (select $pid, * from pg_get_wait_event_trace($pn)) to stdout with csv" >> rings.csv
    $P -d tr -tAc "copy (select $pid, * from pg_wait_event_trace_by_statement($pn)) to stdout with csv" >> by_statement.csv
  done < req-procs.csv
  $P -d tr -tAc "copy (select w.* from pg_stat_get_wait_event_timing() w join pg_stat_activity a using (pid) where a.application_name='req' and w.calls > 0) to stdout with csv" > timing.csv
fi
wait $PB
date +%s.%N > t1
$P -d tr -tAc "copy (select pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-all.csv
$P -d tr -tAc "copy (select left(regexp_replace(s.query,'\s+',' ','g'),60), coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
   from pg_wait_sampling_profile p left join pg_stat_statements s on s.queryid = p.queryid group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-query.csv
$P -d tr -tAc "copy (select ts, pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), queryid from pg_wait_sampling_history) to stdout with csv" > pgws-history.csv
$P -d tr -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),60), calls, round(total_exec_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2), queryid from pg_stat_statements order by 3 desc limit 10) to stdout with csv" > pgss.csv
tail -c +$((LOGPOS+1)) /var/lib/postgresql/pgwet.log > pglog.txt
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*
echo "$TAG done: $(grep -E '^tps' pgb.txt) | still-waiting $(grep -c 'still waiting' pglog.txt) | ring rows $(wc -l < rings.csv 2>/dev/null)"
