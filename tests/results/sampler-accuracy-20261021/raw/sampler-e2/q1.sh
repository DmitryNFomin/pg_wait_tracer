#!/bin/bash
# Q1: pg_wait_sampling under a cgroup CPU quota. 4 pure-CPU victims; truth = 4 x window seconds of CPU*.
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
UNIT=postgresql@18-main.service
CG=/sys/fs/cgroup$(systemctl show -p ControlGroup --value $UNIT)
for q in none 100 50 25; do
  if [ $q = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=${q}%; fi
  sleep 1
  for v in 1 2 3 4; do $P -v secs=80 -f q_victim.sql >/dev/null 2>&1 & done
  sleep 8
  VP=$($P -tAc "select string_agg(pid::text,',') from pg_stat_activity where application_name='qvictim'")
  S0=$(grep throttled_usec $CG/cpu.stat 2>/dev/null | head -1 | awk '{print $2}')
  for p in ${VP//,/ }; do echo "$p $(cat /proc/$p/schedstat)"; done > q-sched0-$q.txt
  T0=$(date +%s.%N); sleep 60; T1=$(date +%s.%N)
  for p in ${VP//,/ }; do echo "$p $(cat /proc/$p/schedstat)"; done > q-sched1-$q.txt
  S1=$(grep throttled_usec $CG/cpu.stat 2>/dev/null | head -1 | awk '{print $2}')
  wait
  $P -tAc "copy (select extract(epoch from ts)::numeric(20,6), pid, coalesce(event,'CPU') from pg_wait_sampling_history
           where ts >= to_timestamp($T0) and ts < to_timestamp($T1)) to stdout with csv" > q-hist-$q.csv
  echo "$q victims=$VP window=$(python3 -c "print(round($T1-$T0,3))") throttled_s=$(python3 -c "print(round(((${S1:-0})-(${S0:-0}))/1e6,2))") cpu.max=$(cat $CG/cpu.max 2>/dev/null)"
  echo "$VP" > q-pids-$q.txt; echo "$T0 $T1" > q-win-$q.txt
done
systemctl set-property --runtime $UNIT CPUQuota=
echo "reverted cpu.max=$(cat $CG/cpu.max 2>/dev/null)"
