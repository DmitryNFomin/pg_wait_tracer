#!/bin/bash
# cc.sh TAG SERVER(18|20) OBS(pgws|tracer|patch) FIO(on|off) [RATE]
# TPC-B at a fixed rate with periodic fio write bursts on the same disk (2 s every 10 s). 120 s window.
set -uo pipefail
TAG=$1; SRV=$2; OBS=$3; FIO=$4; RATE=${5:-1500}
if [ $SRV = 18 ]; then PORT=5418; B=/usr/bin; PMF=/var/lib/postgresql/18/main/postmaster.pid
else PORT=5419; B=/opt/pgwet/bin; PMF=/var/lib/postgresql/pgwet/postmaster.pid; fi
cd /root/t; mkdir -p cc-$TAG; rm -rf cc-$TAG/*; cd cc-$TAG
P="$B/psql -X -q -h /var/run/postgresql -p $PORT -U postgres -d postgres"
PM=$(head -1 $PMF)
python3 -c "import time; print(time.time(), time.clock_gettime(time.CLOCK_MONOTONIC))" > clock0
$P -tAc "checkpoint" >/dev/null
PGAPPNAME=req $B/pgbench -n -h /var/run/postgresql -p $PORT -U postgres -d postgres -M prepared -b tpcb-like -c 16 -j 4 -R $RATE -T 150 \
  -l --log-prefix=lat > pgb.txt 2>&1 & PB=$!
sleep 10
$P -tAc "select pg_wait_sampling_reset_profile(); select pg_stat_statements_reset()" >/dev/null
PGAPPNAME=histpoll timeout 122 $B/psql -X -qAt -h /var/run/postgresql -p $PORT -U postgres -d postgres -f ../histpoll.sql > pgws-hist-poll.csv 2>/dev/null &
date +%s.%N > t0
if [ $FIO = on ]; then
  ( for k in $(seq 12); do sleep 8; s=$(date +%s.%N)
      fio --name=b --filename=/var/tmp/burst.dat --size=2G --rw=randwrite --bs=64k --direct=1 --ioengine=libaio \
          --iodepth=32 --numjobs=4 --runtime=2 --time_based --group_reporting > /dev/null 2>&1
      echo "$s $(date +%s.%N)" >> bursts.txt; done ) & PF=$!
fi
if [ $OBS = tracer ]; then
  /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 120 --view system_event -T trace > tracer-system_event.txt 2>&1
else sleep 120; fi
date +%s.%N > t1
if [ $OBS = patch ]; then
  $P -tAc "copy (select w.pid, w.backend_type, w.procnumber from pg_stat_get_wait_event_timing() w group by 1,2,3) to stdout with csv" > procs.csv
  : > rings.csv
  while IFS=, read pid bt pn; do $P -tAc "copy (select $pid, '$bt', * from pg_get_wait_event_trace($pn)) to stdout with csv" >> rings.csv; done < procs.csv
  $P -tAc "copy (select * from pg_stat_get_wait_event_timing() where calls > 0) to stdout with csv" > timing.csv
  $P -tAc "select (extract(epoch from clock_timestamp())*1e9)::int8" > patch-clock-now.txt
  $P -tAc "select max(timestamp_ns) from pg_get_backend_wait_event_trace()" >> patch-clock-now.txt
fi
$P -tAc "copy (select pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile group by 1,2,3 order by 4 desc) to stdout with csv" > pgws-all.csv
$P -tAc "copy (select pid, backend_type, application_name from pg_stat_activity) to stdout with csv" > activity.csv
python3 -c "import time; print(time.time(), time.clock_gettime(time.CLOCK_MONOTONIC))" > clock1
[ $FIO = on ] && wait $PF
wait $PB
cat lat.* > lat-all.txt 2>/dev/null; rm -f lat.*
[ $OBS = tracer ] && /root/dump_raw trace > raw.txt 2>/dev/null
echo "$TAG done: $(grep -E '^tps' pgb.txt) bursts $(wc -l < bursts.txt 2>/dev/null)"
