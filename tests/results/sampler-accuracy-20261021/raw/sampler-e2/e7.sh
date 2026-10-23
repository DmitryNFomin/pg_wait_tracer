#!/bin/bash
# E7: known-truth victims vs pg_wait_sampling and a pg_stat_activity poller, idle to 8x oversubscribed.
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
$P -c "create unlogged table if not exists e7_truth(run text, pid int, t_start timestamptz, t_end timestamptz, sleep_s float8, iters int)" 2>/dev/null
$P -c "create unlogged table if not exists e7_psa(run text, ts timestamptz, pid int, state text, wet text, we text)" 2>/dev/null
$P -c "create unlogged table if not exists e7_pgws(run text, pid int, event_type text, event text, cnt bigint)" 2>/dev/null
NV=4
for c in 0 4 8 16 32; do
  tag=c$c
  $P -tAc "delete from e7_truth where run='$tag'; delete from e7_psa where run='$tag'; delete from e7_pgws where run='$tag'" >/dev/null
  [ $c -gt 0 ] && { $PGB -S -c $c -j 4 -T 75 > pgb-e7-$tag.txt 2>&1 & PB=$!; sleep 8; }
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  pids=()
  for v in $(seq $NV); do $P -v secs=60 -v run=$tag -f e7_victim.sql >/dev/null & pids+=($!); done
  sleep 0.5
  $P -v secs=58 -v run=$tag -f e7_poller.sql >/dev/null & pids+=($!)
  for v in $(seq $NV); do :; done
  # schedstat of victims, start/end
  sleep 1
  VP=$($P -tAc "select string_agg(pid::text,' ') from pg_stat_activity where application_name='e7victim'")
  for p in $VP; do echo "$p $(cat /proc/$p/schedstat)"; done > sched-start-$tag.txt
  sleep 56
  for p in $VP; do echo "$p $(cat /proc/$p/schedstat 2>/dev/null)"; done > sched-end-$tag.txt
  wait "${pids[@]}"
  $P -tAc "insert into e7_pgws select '$tag', p.pid, coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
           from pg_wait_sampling_profile p where p.pid in (select pid from e7_truth where run='$tag') group by 1,2,3,4" >/dev/null
  [ $c -gt 0 ] && wait $PB
  echo "$tag done $(grep -E '^tps' pgb-e7-$tag.txt 2>/dev/null) load=$(cut -d' ' -f1 /proc/loadavg)"
done
