#!/usr/bin/env python3
"""oracle_share.py <oracle.csv> <win_from_ns> <win_to_ns>

PostgreSQL's own accounting for the window: per-event backend-seconds and the
CPU share of non-idle, from pg_stat_activity sampled by a separate session.
CSV columns: it, ts_ns, pid, state, wait_event_type, wait_event
"""
import sys, csv, collections

path, wf, wt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
cpu = 0
per_event = collections.Counter()
total = 0
iters = {}
rows_total = 0
with open(path) as f:
    for row in csv.reader(f):
        if len(row) < 6:
            continue
        it, ts, pid, state, wet, we = (int(row[0]), int(row[1]), int(row[2]),
                                       row[3], row[4], row[5])
        if ts < wf or ts >= wt:
            continue
        iters[it] = ts
        rows_total += 1
        if state != "active":
            continue
        if wet == "Activity" or we == "ClientRead":
            continue
        total += 1
        if we == "":
            cpu += 1
            per_event["CPU"] += 1
        else:
            per_event[f"{wet}:{we}"] += 1

n = len(iters)
tss = sorted(iters.values())
# each sample row represents the sampler's OWN mean spacing in backend-seconds,
# not window/n (the sampler may not cover the whole window).
w = (tss[-1] - tss[0]) / 1e9 / (n - 1) if n > 1 else 0.0
win_s = (tss[-1] - tss[0]) / 1e9
print(f"ORACLE iters={n} span={win_s:.2f}s spacing={w*1e3:.2f}ms rows={rows_total} "
      f"backends={rows_total/n:.2f} active_samples={total} cpu_samples={cpu} "
      f"share={100.0*cpu/total if total else 0:.1f}%  "
      f"active_frac={100.0*total/rows_total if rows_total else 0:.1f}% "
      f"active_backend_s={total*w:.1f} cpu_backend_s={cpu*w:.1f} "
      f"wait_backend_s={(total-cpu)*w:.1f}")
for k, v in per_event.most_common(12):
    print(f"  {k:28s} {100.0*v/total if total else 0:6.1f}%  {v*w:8.2f}s")
