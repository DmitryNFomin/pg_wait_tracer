#!/bin/bash
# c2.sh TAG RANGE TIMEOUT_MS [OBS=full|pgws]: 60 s window of lookups with statement_timeout; caller sets fio/bloat.
set -uo pipefail
TAG=$1; RANGE=$2; TMO=$3; OBS=${4:-full}
cd /root/t; mkdir -p c2-$TAG; rm -rf c2-$TAG/*; cd c2-$TAG
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d c3"
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
python3 ../client.py req 80 $RANGE $TMO 8 & PC=$!
sleep 10
BP=$($P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='app'")
echo $BP > client-pids
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null; $P -tAc "select pg_stat_reset_shared('io')" >/dev/null
$P -tAc "copy (select b.pid, x.reads, x.read_time from unnest('{$BP}'::int[]) b(pid), pg_stat_get_backend_io(b.pid) x where x.object='relation' and x.context='normal') to stdout with csv" > bio0.csv
date +%s.%N > t0
if [ $OBS = full ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 60 --view system_event -T trace > tracer-system_event.txt 2>&1
else sleep 60; fi
date +%s.%N > t1
$P -tAc "copy (select b.pid, x.reads, x.read_time from unnest('{$BP}'::int[]) b(pid), pg_stat_get_backend_io(b.pid) x where x.object='relation' and x.context='normal') to stdout with csv" > bio1.csv
$P -tAc "copy (select coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         where pid in ($BP) group by 1,2 order by 3 desc) to stdout with csv" > pgws.csv
$P -tAc "copy (select queryid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         where pid in ($BP) group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-qid.csv
$P -tAc "copy (select queryid, calls, shared_blks_hit, shared_blks_read, round(shared_blk_read_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,1)
         from pg_stat_statements where query like 'SELECT filler FROM t WHERE id%') to stdout with csv" > pgss.csv
$P -tAc "copy (select reads, round(read_time::numeric,1) from pg_stat_io where object='relation' and backend_type='client backend' and reads>0) to stdout with csv" > pgio.csv
wait $PC
cat req.* > req-all.txt; rm -f req.*
if [ $OBS = full ]; then
  /root/pg_wait_tracer --replay -T trace --view histogram -e IO:DataFileRead > tracer-hist.txt 2>&1
  /root/pg_wait_tracer --replay -T trace --view query_event > tracer-query.txt 2>&1
fi
echo "$TAG done: requests $(wc -l < req-all.txt), timeouts $(grep -c timeout req-all.txt)"
