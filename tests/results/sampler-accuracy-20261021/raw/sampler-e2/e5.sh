#!/bin/bash
# E5: same total waiting, different event histories; what does pg_wait_sampling's profile say?
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
$P -c "create unlogged table if not exists e5_result(run text, mode text, pid int, waits int, sleep_s float8)" 2>/dev/null
$P -c "alter table e5_result add column if not exists run text" >/dev/null 2>&1
for tag in idle tpcb-c8; do
  [ $tag = tpcb-c8 ] && { $PGB -c 8 -j 4 -T 45 > pgb-e5.txt 2>&1 & PB=$!; sleep 8; }
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  $P -v mode=long  -f e5.sql & A=$!
  $P -v mode=short -f e5.sql & B=$!
  wait $A $B
  $P -tAc "update e5_result set run='$tag' where run is null"
  [ $tag = tpcb-c8 ] && wait $PB
  echo "== $tag"
  $P -c "select r.mode, r.waits as true_waits, round(r.sleep_s::numeric,2) as true_wait_s,
           coalesce(sum(p.count) filter (where p.event='PgSleep'),0) as sampled_pgsleep,
           coalesce(sum(p.count),0) as sampled_all
         from e5_result r left join pg_wait_sampling_profile p on p.pid=r.pid
         where r.run='$tag' group by 1,2,3 order by 1"
done
