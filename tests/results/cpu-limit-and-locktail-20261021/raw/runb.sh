#!/bin/bash
# runb.sh TAG LIMIT(none|NN%) RATE OBS(pgws|tracer) [DUR] [pgbench args...]
# Fixed-rate pgbench (16 clients, outside the cgroup) against PG18 in its systemd cgroup with an optional CPU limit.
# Records: per-txn latency, pg_wait_sampling profile (client backends), kernel schedstat per backend, cgroup cpu.stat,
# and (OBS=tracer) a pg_wait_tracer exact trace, all over the same 60 s window.
set -uo pipefail
TAG=$1; LIM=$2; RATE=$3; OBS=$4; DUR=${5:-60}; shift 5 || shift $#
EXTRA="$*"
cd /root/case-ab; mkdir -p b-$TAG; rm -rf b-$TAG/*; cd b-$TAG
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d postgres"
UNIT=postgresql@18-main.service
CG=/sys/fs/cgroup$(systemctl show -p ControlGroup --value $UNIT)
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
if [ $LIM = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=$LIM; fi
sleep 1; cat $CG/cpu.max > cpu.max
LOGPOS=$(stat -c %s /var/log/postgresql/postgresql-18-main.log)
pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d postgres -M prepared -c 16 -j 4 -R $RATE -T $((DUR+20)) \
  -l --log-prefix=lat $EXTRA > pgb.txt 2>&1 & PB=$!
PR=""
if [ -n "${REPORT_RATE:-}" ]; then
  PGOPTIONS="${REPORT_PGOPTIONS:-}" PGAPPNAME=report pgbench -n -h /var/run/postgresql -p 5418 -U postgres -d postgres -c 1 -R $REPORT_RATE -T $((DUR+20)) \
    -f ../report.sql -l --log-prefix=rep > pgb-report.txt 2>&1 & PR=$!
fi
sleep 10
BP=$($P -tAc "select string_agg(pid::text,' ') from pg_stat_activity where application_name like 'pgbench%' and backend_type='client backend'")
for p in $BP; do echo "$p $(cat /proc/$p/schedstat)"; done > sched0
cat $CG/cpu.stat > cg0
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null
date +%s.%N > t0
if [ $OBS = sampled ]; then
  /root/pg_wait_tracer --pid $PM --mode sampled --sample-rate 100 --count 1 --interval $DUR --view time_model -T trace > tracer-time_model.txt 2>&1
elif [ $OBS = tracer ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval $DUR --view system_event -T trace > tracer-system_event.txt 2>&1
else sleep $DUR; fi
date +%s.%N > t1
for p in $BP; do echo "$p $(cat /proc/$p/schedstat 2>/dev/null)"; done > sched1
cat $CG/cpu.stat > cg1
$P -tAc "copy (select pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-all.csv
$P -tAc "copy (select left(regexp_replace(s.query,'\s+',' ','g'),70), coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
         from pg_wait_sampling_profile p left join pg_stat_statements s on s.queryid = p.queryid
         where p.pid in ($(echo $BP | tr ' ' ',')) group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-by-query.csv
$P -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),70), calls, round(total_exec_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2)
         from pg_stat_statements order by 3 desc limit 15) to stdout with csv" > pgss.csv
echo "$BP" > client-pids
wait $PB $PR
tail -c +$((LOGPOS+1)) /var/log/postgresql/postgresql-18-main.log > pglog.txt
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*; cat rep.* > rep-all.txt 2>/dev/null; rm -f rep.*
if [ $OBS = tracer ]; then
  /root/pg_wait_tracer --replay -T trace --view query_event > tracer-query.txt 2>&1
fi
systemctl set-property --runtime $UNIT CPUQuota=
echo "$TAG done: $(grep -E '^tps|latency average|latency stddev' pgb.txt | tr '\n' ' ')"
