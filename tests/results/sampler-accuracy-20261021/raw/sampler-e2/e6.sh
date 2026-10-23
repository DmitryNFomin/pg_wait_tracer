#!/bin/bash
# E6: injected short advisory-lock waits; did pg_wait_sampling (100 Hz history) see each one?
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
$P -c "create unlogged table if not exists e6_waits(run text, pid int, t0 timestamptz, t1 timestamptz)" 2>/dev/null
for tag in idle tpcb-c8; do
  $P -tAc "delete from e6_waits where run='$tag'"
  [ $tag = tpcb-c8 ] && { $PGB -c 8 -j 4 -T 135 > pgb-e6.txt 2>&1 & PB=$!; sleep 8; }
  $P -v role=holder -v secs=120 -v run=$tag -f e6.sql & H=$!
  sleep 0.5
  $P -v role=victim -v secs=118 -v run=$tag -f e6.sql & V=$!
  wait $H $V
  [ $tag = tpcb-c8 ] && wait $PB
  sleep 1
  $P -tAc "copy (
    with w as (select pid, t0, t1, extract(epoch from t1-t0)*1000 as ms,
                      lag(t1) over (order by t0) as prev_t1
               from e6_waits where run='$tag')
    select ms,
           exists (select 1 from pg_wait_sampling_history h
                   where h.pid=w.pid and h.event='advisory'
                     and h.ts between w.t0 - interval '1 ms' and w.t1) as seen
    from w where ms > 0.05 and (prev_t1 is null or w.t0 - prev_t1 > interval '1 ms')
  ) to stdout with csv" > e6-$tag.csv
  echo "$tag waits=$(wc -l < e6-$tag.csv)"
done
