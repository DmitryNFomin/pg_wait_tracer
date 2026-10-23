# Draft test plan: sampler timing and bias under load (gate-2)

Box: gate-2, Hetzner, 4 vCPU AMD EPYC-Rome, 7 GB, Ubuntu 24.04, kernel 6.8.
PostgreSQL 18 on port 5418, shared_buffers 128MB, pgbench scale 10 (1M rows, ~150 MB, so it does
not fit shared_buffers but fits the OS page cache). PG 13/16/17 clusters also run; idle during tests.
Shared box: every run holds `flock /tmp/pgwt-box-check.lock`; config changes are reverted at the end.

Goal: evidence that is about SAMPLING IN GENERAL, not about pg_wait_tracer's own sampler, for two talk claims:
(a) under load the sampler's ticks run late (it is a normal process competing for a core);
(b) late ticks are biased: a core frees up when a backend blocks, so samples land in waits more and CPU less.
Plus (c): measure wait-state changes per second on this box, to back the "1 photo per 50 changes" slide.

## E1. Bare timer lateness (control, no PostgreSQL code involved)
A 40-line C program: SCHED_OTHER, nice 0, clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) every 10 ms (100 Hz)
for 60 s, records lateness = wake time - target per tick.
Loads: idle; pgbench -S -c 4 (= vCPU); -S -c 8; -S -c 16; TPC-B -c 8.
Report per load: p50, p90, p99, p99.9, max lateness; % ticks > 1 ms late; achieved rate.

## E2. pg_wait_sampling's own collector (the real tool, default settings)
Install postgresql-18-pg-wait-sampling (PGDG 1.1.11), add to shared_preload_libraries, restart PG18.
Defaults: history_period = 10 ms, profile_period = 10 ms. Raise history_size so a 60 s window fits.
Under the same loads, read pg_wait_sampling_history (ts per sample). Collector rounds = distinct ts.
Report inter-round interval distribution (p50/p99/max), achieved rate, and rounds missing vs 100 Hz.

## E3. Share accuracy: pg_wait_sampling vs exact trace, same window
Run pg_wait_tracer --mode full (exact, watchpoint per transition) and pg_wait_sampling together for 60 s
under each load. Compare per-event share of DB time, including CPU (wait_event NULL), and AAS.
Also report exact transitions per second (validates the 40,000 / 220,000 per second figures).
Caveat: the exact tier's overhead perturbs the workload; both tools observe the same perturbed run.

## E4. Direct bias test: is a sample's state right at its own timestamp, and is a late sample biased?
For each pg_wait_sampling history row (ts, pid, event), look up the exact state of that pid at ts in the
trace (wall-clock alignment via the trace's mono->wall mapping; drop samples within +-0.5 ms of a transition).
Report: confusion matrix (sampled CPU/wait vs exact CPU/wait); and P(sample = wait) as a function of the
collector round's lateness (on-time vs late buckets). Prediction if (b) holds: late rounds see waits more often
than on-time rounds, beyond what the exact state mix at random times gives.

## Cleanup
Remove pg_wait_sampling from shared_preload_libraries, restart PG18, keep the package (or purge), release lock.
