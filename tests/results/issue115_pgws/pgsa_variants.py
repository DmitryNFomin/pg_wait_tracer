#!/usr/bin/env python3
"""pgsa_variants.py <oracle.csv> <from_ns> <to_ns> [--pids p,p,..] [--label L]

The pg_stat_activity reference under several classifier choices, over one
window and one pid set. Prints one machine-readable line per variant:

  V1  conventional (both #115 investigations): keep state='active' rows,
      drop wait_event_type='Activity' and wait_event='ClientRead';
      CPU := wait_event IS NULL.  Non-active rows are dropped from BOTH
      numerator and denominator.
  V2  V1 + the non-active/no-wait-event rows counted as CPU (the backend is
      between pgstat_report_activity(idle) and the next ClientRead: it is on
      a processor doing protocol work, not waiting on anything).
  V3  V1 + torn rows (state='active' AND wait_event='ClientRead') counted as
      CPU instead of dropped.
  V4  V2 + V3 together.
"""
import sys, csv, collections

path = sys.argv[1]
wf = int(sys.argv[2])
wt = int(sys.argv[3])
pids = None
label = ""
a = sys.argv[4:]
while a:
    if a[0] == "--pids":
        pids = set(int(x) for x in a[1].split(",") if x.strip())
        a = a[2:]
    elif a[0] == "--label":
        label = a[1]
        a = a[2:]
    else:
        a = a[1:]

iters = set()
rows = 0
c = collections.Counter()
per_ev = collections.Counter()
for row in csv.reader(open(path)):
    if len(row) < 6:
        continue
    it, ts, pid, state, wet, we = (int(row[0]), int(row[1]), int(row[2]),
                                   row[3], row[4], row[5])
    if ts < wf or ts >= wt:
        continue
    iters.add(it)
    if pids is not None and pid not in pids:
        continue
    rows += 1
    if state == "active":
        if we == "ClientRead":
            c["torn"] += 1
        elif wet == "Activity":
            c["act_activity"] += 1
        elif we == "":
            c["act_cpu"] += 1
            per_ev["CPU"] += 1
        else:
            c["act_wait"] += 1
            per_ev[f"{wet}:{we}"] += 1
    else:
        if we == "ClientRead":
            c["idle_clientread"] += 1
        elif wet == "Activity":
            c["idle_activity"] += 1
        elif we == "":
            c["nonactive_nowait"] += 1
        else:
            c["nonactive_wait"] += 1

n = len(iters)
win = (wt - wf) / 1e9
w = win / n if n else 0.0   # backend-seconds represented by one sample row


def emit(name, cpu, tot):
    sh = 100.0 * cpu / tot if tot else 0.0
    print(f"PGSA_{name:2s} {label:10s} share={sh:5.2f}%  cpu_s={cpu*w:8.2f} "
          f"nonidle_s={tot*w:8.2f} cpu_n={cpu} nonidle_n={tot}")


act_cpu, act_wait = c["act_cpu"], c["act_wait"]
torn, nnw = c["torn"], c["nonactive_nowait"]
print(f"PGSA_IN {label:10s} iters={n} ({n/win:.1f} Hz) rows={rows} "
      f"backends={rows/n if n else 0:.2f} win={win:.3f}s "
      f"act_cpu={act_cpu} act_wait={act_wait} "
      f"torn={torn}({100.0*torn/rows if rows else 0:.2f}%) "
      f"nonactive_nowait={nnw}({100.0*nnw/rows if rows else 0:.2f}%) "
      f"idle_clientread={c['idle_clientread']} idle_activity={c['idle_activity']} "
      f"act_activity={c['act_activity']} nonactive_wait={c['nonactive_wait']}")
emit("V1", act_cpu, act_cpu + act_wait)
emit("V2", act_cpu + nnw, act_cpu + nnw + act_wait)
emit("V3", act_cpu + torn, act_cpu + torn + act_wait)
emit("V4", act_cpu + nnw + torn, act_cpu + nnw + torn + act_wait)
den = act_cpu + act_wait
for k, v in per_ev.most_common(10):
    print(f"    {label:10s} {k:26s} {100.0*v/den if den else 0:6.2f}%  {v*w:8.2f}s")
