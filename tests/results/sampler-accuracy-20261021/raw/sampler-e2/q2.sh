#!/bin/bash
# Q2: mixed victims (1 ms CPU + 1 ms pg_sleep) under a cgroup CPU quota: share AND volume vs truth.
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
UNIT=postgresql@18-main.service
trap 'systemctl set-property --runtime $UNIT CPUQuota= ; systemctl show -p CPUQuotaPerSecUSec $UNIT' EXIT
for q in none 100 50; do
  tag=q$q
  if [ $q = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=${q}%; fi
  sleep 1
  $P -tAc "delete from e7_truth where run='$tag'; delete from e7_pgws where run='$tag'" >/dev/null
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  for v in 1 2 3 4; do $P -v secs=60 -v run=$tag -f e7_victim.sql >/dev/null 2>&1 & done
  wait
  $P -tAc "insert into e7_pgws select '$tag', p.pid, coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
           from pg_wait_sampling_profile p where p.pid in (select pid from e7_truth where run='$tag') group by 1,2,3,4" >/dev/null
  echo "$tag done"
done
$P -c "with t as (select run, sum(sleep_s) s, sum(extract(epoch from t_end-t_start)) w from e7_truth where run like 'q%' group by run),
            p as (select run, sum(cnt) filter (where event='PgSleep') s, sum(cnt) filter (where event='CPU') c, sum(cnt) tot from e7_pgws where run like 'q%' group by run)
       select run, round(t.w::numeric,0) truth_session_s, round((p.tot*0.01)::numeric,1) pgws_session_s,
              round((100*p.tot*0.01/t.w)::numeric,0) pct_of_truth,
              round((100*t.s/t.w)::numeric,1) truth_sleep_pct, round((100.0*p.s/p.tot)::numeric,1) pgws_sleep_pct
       from t join p using (run) order by run"
