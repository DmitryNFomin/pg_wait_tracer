#!/bin/bash
# Case CL: pgbench TPC-B (16 clients) under a cgroup CPU limit. pg_wait_sampling (inside the cgroup) vs
# pg_wait_tracer exact tier (daemon outside) vs kernel schedstat, same 60 s window.
set -uo pipefail
cd /root/case-cl
P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d postgres"
PGB="pgbench -h /var/run/postgresql -p 5418 -U postgres -d postgres"
UNIT=postgresql@18-main.service
CG=/sys/fs/cgroup$(systemctl show -p ControlGroup --value $UNIT)
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
trap 'systemctl set-property --runtime $UNIT CPUQuota= ; systemctl show -p CPUQuotaPerSecUSec $UNIT' EXIT
for lim in none 200 100; do for obs in pgws tracer; do
  tag=$lim-$obs
  if [ $lim = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=${lim}%; fi
  sleep 1
  $PGB -c 16 -j 4 -T 85 -P 10 > pgb-$tag.txt 2>&1 & PB=$!
  sleep 15
  BP=$($P -tAc "select string_agg(pid::text,' ') from pg_stat_activity where application_name='pgbench'")
  for p in $BP; do echo "$p $(cat /proc/$p/schedstat)"; done > sched0-$tag.txt
  TH0=$(awk '/throttled_usec/{print $2}' $CG/cpu.stat 2>/dev/null); TH0=${TH0:-0}
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  T0=$(date +%s.%N)
  if [ $obs = tracer ]; then
    /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 60 > tracer-$tag.txt 2>&1
  else
    sleep 60
  fi
  T1=$(date +%s.%N)
  for p in $BP; do echo "$p $(cat /proc/$p/schedstat 2>/dev/null)"; done > sched1-$tag.txt
  TH1=$(awk '/throttled_usec/{print $2}' $CG/cpu.stat 2>/dev/null); TH1=${TH1:-0}
  $P -tAc "copy (select coalesce(event_type,'CPU'), coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile
           where pid in ($(echo $BP | tr ' ' ',')) group by 1,2 order by 3 desc) to stdout with csv" > pgws-$tag.csv
  $P -tAc "copy (select extract(epoch from ts)::numeric(20,6) from pg_wait_sampling_history
           where ts >= to_timestamp($T0) and ts < to_timestamp($T1) group by 1 order by 1) to stdout" > rounds-$tag.txt
  wait $PB
  echo "$T0 $T1 $(( ${TH1:-0} - ${TH0:-0} ))" > win-$tag.txt
  echo "$tag done: $(grep -E '^tps|latency average' pgb-$tag.txt | tr '\n' ' ')"
done; done
