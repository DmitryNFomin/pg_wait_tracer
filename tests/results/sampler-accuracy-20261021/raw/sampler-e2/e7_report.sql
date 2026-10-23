with t as (select run, sum(sleep_s) sleep_s, sum(extract(epoch from t_end - t_start)) wall_s, sum(iters) iters from e7_truth group by run),
     w as (select run, sum(cnt) filter (where event='PgSleep') s, sum(cnt) filter (where event='CPU') c, sum(cnt) tot from e7_pgws group by run),
     a as (select run, count(*) rows,
                  count(*) filter (where state='active' and we='PgSleep') act_sleep,
                  count(*) filter (where state='active' and we is null) act_cpu,
                  count(*) filter (where state='active') act,
                  count(*) filter (where state is distinct from 'active') nonactive
           from e7_psa group by run)
select t.run,
       round((100*t.sleep_s/t.wall_s)::numeric,1)            as truth_sleep_pct,
       round((100.0*w.s/w.tot)::numeric,1)                    as pgws_sleep_pct,
       round((100.0*a.act_sleep/nullif(a.act,0))::numeric,1)  as psa_sleep_pct_active,
       a.rows as psa_rows, round((100.0*a.nonactive/a.rows)::numeric,1) as psa_nonactive_pct,
       w.tot as pgws_samples, t.iters
from t join w using (run) join a using (run) order by length(t.run), t.run;
