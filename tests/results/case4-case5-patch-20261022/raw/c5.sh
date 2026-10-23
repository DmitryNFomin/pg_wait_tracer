#!/bin/bash
# c5.sh TAG LIMIT(none|NN%) RATE CAPTURE(off|trace) [DUR]: case 5 (CPU limit + periodic parallel report) on the
# patched build (port 5419, transient unit pgwet.service). Same workload as runb.sh: pgbench TPC-B, 16 clients outside
# the cgroup, fixed rate; report every ~5 s when REPORT_RATE is set. Ground truth: /proc/<pid>/schedstat, cgroup cpu.stat.
# With CAPTURE=trace, client rings are dumped once right after the window (ring wrap is checked in c5rep.py).
set -uo pipefail
TAG=$1; LIM=$2; RATE=$3; CAP=$4; DUR=${5:-60}
B=/opt/pgwet/bin
export PGOPTIONS="-c pg_wait_event_tracing.capture=off"   # observer sessions are not traced
cd /root/t; mkdir -p c5-$TAG; rm -rf c5-$TAG/*; cd c5-$TAG
P="$B/psql -X -q -h /var/run/postgresql -p 5419 -U postgres -d postgres"
UNIT=pgwet.service
CG=/sys/fs/cgroup$(systemctl show -p ControlGroup --value $UNIT)
$P -c "alter database postgres set pg_wait_event_tracing.capture = '$CAP'" -c "select pg_stat_clear_orphaned_wait_event_rings()" >/dev/null
if [ $LIM = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=$LIM; fi
sleep 1; cat $CG/cpu.max > cpu.max
python3 -c "import time; print(time.time(), time.clock_gettime_ns(time.CLOCK_MONOTONIC))" > clock0
env -u PGOPTIONS $B/pgbench -n -h /var/run/postgresql -p 5419 -U postgres -d postgres -M prepared -c 16 -j 4 -R $RATE -T $((DUR+35)) \
  -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
PR=""
if [ -n "${REPORT_RATE:-}" ]; then
  PGOPTIONS="${REPORT_PGOPTIONS:-}" PGAPPNAME=report $B/pgbench -n -h /var/run/postgresql -p 5419 -U postgres -d postgres -c 1 -R $REPORT_RATE -T $((DUR+35)) \
    -f ../report.sql -l --log-prefix=rep > pgb-report.txt 2>&1 & PR=$!
fi
sleep 10
BP=$($P -tAc "select string_agg(pid::text,' ') from pg_stat_activity where application_name like 'pgbench%' and backend_type='client backend'")
echo "$BP" > client-pids
$P -tAc "copy (select distinct w.pid, w.procnumber from pg_stat_get_wait_event_timing() w where w.pid in ($(echo $BP | tr ' ' ','))) to stdout with csv" > procs.csv
for p in $BP; do echo "$p $(cat /proc/$p/schedstat)"; done > sched0
cat $CG/cpu.stat > cg0
$P -tAc "copy (select w.* from pg_stat_get_wait_event_timing() w where w.pid in ($(echo $BP | tr ' ' ',')) and w.calls > 0) to stdout with csv" > timing0.csv
$P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null; $P -tAc "select pg_stat_statements_reset()" >/dev/null
date +%s.%N > t0
dump() { while IFS=, read pid pn; do $P -tAc "copy (select $pid, * from pg_get_wait_event_trace($pn)) to stdout with csv" >> rings-raw.csv; done < procs.csv; }
sleep $DUR
date +%s.%N > t1
for p in $BP; do echo "$p $(cat /proc/$p/schedstat 2>/dev/null)"; done > sched1
cat $CG/cpu.stat > cg1
$P -tAc "copy (select w.* from pg_stat_get_wait_event_timing() w where w.pid in ($(echo $BP | tr ' ' ',')) and w.calls > 0) to stdout with csv" > timing1.csv
$P -tAc "copy (select pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
         group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-all.csv
$P -tAc "copy (select left(regexp_replace(query,'\s+',' ','g'),70), calls, round(total_exec_time::numeric,1), round(mean_exec_time::numeric,3), round(max_exec_time::numeric,2)
         from pg_stat_statements order by 3 desc limit 15) to stdout with csv" > pgss.csv
[ $CAP = trace ] && dump   # one dump after the window: a 4 MB ring holds ~70 s at this rate; the dump runs inside the cgroup
wait $PB $PR
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*; cat rep.* > rep-all.txt 2>/dev/null; rm -f rep.*
[ -f rings-raw.csv ] && sort -t, -k1,1n -k3,3n -u rings-raw.csv > rings.csv && rm rings-raw.csv
systemctl set-property --runtime $UNIT CPUQuota=
echo "$TAG done: $(grep -E '^tps|latency average' pgb.txt | tr '\n' ' ')"
