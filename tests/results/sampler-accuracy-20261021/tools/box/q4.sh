#!/bin/bash
# Q4: same 50% quota + 4 pure-CPU victims, observed by pg_wait_tracer (daemon outside PG's cgroup).
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
UNIT=postgresql@18-main.service
trap 'systemctl set-property --runtime $UNIT CPUQuota= ; systemctl show -p CPUQuotaPerSecUSec $UNIT' EXIT
PM=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
cat /proc/self/cgroup
for mode in sampled full; do
  systemctl set-property --runtime $UNIT CPUQuota=50%
  sleep 1
  for v in 1 2 3 4; do $P -v secs=85 -f q_victim.sql >/dev/null 2>&1 & done
  sleep 8
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  if [ $mode = sampled ]; then
    /root/pg_wait_tracer --pid $PM --mode sampled --sample-rate 100 --count 1 --interval 60 > q4-$mode.txt 2>&1
  else
    /root/pg_wait_tracer --pid $PM --mode full --count 1 --interval 60 > q4-$mode.txt 2>&1
  fi
  $P -tAc "select coalesce(event,'CPU'), sum(count) from pg_wait_sampling_profile p join pg_stat_activity a using (pid)
           where a.application_name='qvictim' group by 1" > q4-pgws-$mode.txt
  wait
  echo "== $mode"; head -40 q4-$mode.txt; echo "-- pg_wait_sampling over the same ~60 s (victims): "; cat q4-pgws-$mode.txt
done
