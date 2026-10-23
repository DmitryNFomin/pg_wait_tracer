#!/bin/bash
# c4.sh TAG RANGE TIMEOUT_MS CAPTURE(off|trace): case 4 (slow disk vs bloat) on the patched build (port 5419).
# Same workload as c2.sh (8 psycopg workers, closed loop, PK lookups). The caller sets fio / bloat.
# With CAPTURE=trace, every client backend's ring is dumped every 5 s (dedup on pid+seq later),
# plus pg_wait_event_trace_by_statement() and pg_stat_get_wait_event_timing() at the end.
set -uo pipefail
TAG=$1; RANGE=$2; TMO=$3; CAP=$4
B=/opt/pgwet/bin
export PGOPTIONS="-c pg_wait_event_tracing.capture=off"   # observer sessions are not traced
cd /root/t; mkdir -p c4-$TAG; rm -rf c4-$TAG/*; cd c4-$TAG
P="$B/psql -X -q -h /var/run/postgresql -p 5419 -U postgres -d c3"
$P -c "alter database c3 set pg_wait_event_tracing.capture = '$CAP'" -c "select pg_stat_clear_orphaned_wait_event_rings()" >/dev/null
python3 -c "import time; print(time.time(), time.clock_gettime_ns(time.CLOCK_MONOTONIC))" > clock0
env -u PGOPTIONS PORT=5419 python3 ../client.py req 95 $RANGE $TMO 8 & PC=$!
sleep 10
BP=$($P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='app'")
echo $BP > client-pids
$P -tAc "copy (select distinct w.pid, w.procnumber from pg_stat_get_wait_event_timing() w where w.pid in ($BP)) to stdout with csv" > procs.csv
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null; $P -tAc "select pg_stat_reset_shared('io')" >/dev/null
$P -tAc "copy (select w.* from pg_stat_get_wait_event_timing() w where w.pid in ($BP) and w.calls > 0) to stdout with csv" > timing0.csv
date +%s.%N > t0
dump() { while IFS=, read pid pn; do $P -tAc "copy (select $pid, * from pg_get_wait_event_trace($pn)) to stdout with csv" >> rings-raw.csv; done < procs.csv; }
END=$(( $(date +%s) + 60 ))
if [ $CAP = trace ]; then while [ $(date +%s) -lt $END ]; do sleep $(( END - $(date +%s) < 5 ? END - $(date +%s) : 5 )); dump; done; else sleep 60; fi
date +%s.%N > t1
$P -tAc "copy (select w.* from pg_stat_get_wait_event_timing() w where w.pid in ($BP) and w.calls > 0) to stdout with csv" > timing1.csv
if [ $CAP = trace ]; then
  : > by_statement.csv
  while IFS=, read pid pn; do $P -tAc "copy (select $pid, * from pg_wait_event_trace_by_statement($pn)) to stdout with csv" >> by_statement.csv; done < procs.csv
fi
$P -tAc "copy (select coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         where pid in ($BP) group by 1,2 order by 3 desc) to stdout with csv" > pgws.csv
$P -tAc "copy (select queryid, calls, shared_blks_hit, shared_blks_read, round(shared_blk_read_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,1)
         from pg_stat_statements where query like 'SELECT filler FROM t WHERE id%') to stdout with csv" > pgss.csv
$P -tAc "copy (select reads, round(read_time::numeric,1) from pg_stat_io where object='relation' and backend_type='client backend' and reads>0) to stdout with csv" > pgio.csv
wait $PC
cat req.* > req-all.txt; rm -f req.*
[ -f rings-raw.csv ] && sort -t, -k1,1n -k3,3n -u rings-raw.csv > rings.csv && rm rings-raw.csv
echo "$TAG done: requests $(wc -l < req-all.txt), timeouts $(grep -c timeout req-all.txt)"
