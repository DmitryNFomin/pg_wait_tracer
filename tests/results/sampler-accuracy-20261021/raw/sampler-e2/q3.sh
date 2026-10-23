#!/bin/bash
# Q3: Q2 repeated 3x per quota, plus a pg_stat_activity poller (SQL sampler, also inside the cgroup).
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
UNIT=postgresql@18-main.service
trap 'systemctl set-property --runtime $UNIT CPUQuota= ; systemctl show -p CPUQuotaPerSecUSec $UNIT' EXIT
for rep in 1 2 3; do for q in none 100 50; do
  tag=r${rep}q$q
  if [ $q = none ]; then systemctl set-property --runtime $UNIT CPUQuota= ; else systemctl set-property --runtime $UNIT CPUQuota=${q}%; fi
  sleep 1
  $P -tAc "delete from e7_truth where run='$tag'; delete from e7_pgws where run='$tag'; delete from e7_psa where run='$tag'" >/dev/null
  $P -tAc "select pg_wait_sampling_reset_profile()" >/dev/null
  for v in 1 2 3 4; do $P -v secs=60 -v run=$tag -f e7_victim.sql >/dev/null 2>&1 & done
  sleep 0.5; $P -v secs=58 -v run=$tag -f e7_poller.sql >/dev/null 2>&1 &
  wait
  $P -tAc "insert into e7_pgws select '$tag', p.pid, coalesce(p.event_type,'CPU'), coalesce(p.event,'CPU'), sum(p.count)
           from pg_wait_sampling_profile p where p.pid in (select pid from e7_truth where run='$tag') group by 1,2,3,4" >/dev/null
done; done
$P -c "with t as (select run, sum(sleep_s) s, sum(extract(epoch from t_end-t_start)) w from e7_truth where run like 'r_q%' group by run),
            p as (select run, sum(cnt) filter (where event='PgSleep') s, sum(cnt) tot from e7_pgws where run like 'r_q%' group by run),
            a as (select run, count(*) rows, count(*) filter (where we='PgSleep') s,
                         min(ts) t0, max(ts) t1 from e7_psa where run like 'r_q%' group by run)
       select substr(run,3) quota, run,
              round((100*p.tot*0.01/t.w)::numeric,0) pgws_pct_of_true_time,
              round((100*t.s/t.w)::numeric,1) truth_sleep, round((100.0*p.s/p.tot)::numeric,1) pgws_sleep,
              round((100.0*a.s/a.rows)::numeric,1) psa_sleep,
              round((a.rows/4.0/extract(epoch from a.t1-a.t0))::numeric,1) psa_hz
       from t join p using (run) join a using (run) order by 1, 2"
