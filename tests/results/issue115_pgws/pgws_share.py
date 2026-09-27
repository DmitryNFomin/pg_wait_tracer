#!/usr/bin/env python3
"""pgws_share.py <pgws.csv> <from_ns> <to_ns> [--pids p,p,..] [--label L]

The pg_wait_sampling reference over one window and one backend set.

Input is a series of snapshots of pg_wait_sampling_profile:
    ts_ns,pid,event_type,event,count      (count is CUMULATIVE)
taken at a fixed cadence by the capture script.  The window's per-event
sample counts are the difference between the last snapshot at or before
`to_ns` and the last snapshot at or before `from_ns`.

Classifier.  pg_wait_sampling stores only PGPROC->wait_event_info; it has no
`state` column, so it cannot tell an active backend on a processor from an
idle backend that is between pgstat_report_activity(idle) and the next
ClientRead.  With pg_wait_sampling.sample_cpu = on (the default) both land in
the NULL bucket.  Its CPU share is therefore structurally a WHOLE-BACKEND
number -- the same thing pgsa_variants.py calls V2 -- and must be compared
against V2 / the whole-backend exact tier / the kernel, not against V1.

  PGWS      CPU := NULL event; non-idle := CPU + every wait but Client:ClientRead
  PGWS_NOCR as above but Client:* dropped entirely (identical in practice)
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

# snapshots[ts][(pid, et, ev)] = cumulative count
snaps = collections.defaultdict(dict)
for row in csv.reader(open(path)):
    if len(row) < 5:
        continue
    try:
        ts, pid, et, ev, cnt = (int(row[0]), int(row[1]), row[2], row[3],
                                int(row[4]))
    except ValueError:
        continue
    if pids is not None and pid not in pids:
        continue
    snaps[ts][(pid, et, ev)] = cnt

times = sorted(snaps)
if len(times) < 2:
    print(f"PGWS_IN {label:10s} ERROR: {len(times)} snapshots, need >= 2")
    sys.exit(1)


def pick(t, prefer_le=True):
    """Snapshot nearest to t (the largest <= t, else the smallest)."""
    le = [x for x in times if x <= t]
    if le and prefer_le:
        return le[-1]
    ge = [x for x in times if x >= t]
    return ge[0] if ge else times[-1]


t0 = pick(wf)
t1 = pick(wt)
if t1 <= t0:
    print(f"PGWS_IN {label:10s} ERROR: snapshot span empty "
          f"(t0={t0} t1={t1} wf={wf} wt={wt})")
    sys.exit(1)

span = (t1 - t0) / 1e9
want_win = (wt - wf) / 1e9
a0, a1 = snaps[t0], snaps[t1]
d = collections.Counter()
for k, v in a1.items():
    d[k] = v - a0.get(k, 0)

per_ev = collections.Counter()
per_pid = collections.Counter()
cpu = 0
wait = 0
idle = 0
for (pid, et, ev), n in d.items():
    if n <= 0:
        continue
    per_pid[pid] += n
    if ev == "" or ev is None or et == "":
        cpu += n
        per_ev["CPU"] += n
    elif ev == "ClientRead":
        idle += n
        per_ev["IDLE:ClientRead"] += n
    else:
        wait += n
        per_ev[f"{et}:{ev}"] += n

npid = len(per_pid) or 1
# every probe walks every backend, so samples-per-backend is the probe rate
probe_n = (cpu + wait + idle) / npid
rate = probe_n / span if span else 0.0
w = span / probe_n if probe_n else 0.0   # backend-seconds per sample row

tot = cpu + wait
share = 100.0 * cpu / tot if tot else 0.0
print(f"PGWS_IN {label:10s} snaps={len(times)} span={span:.3f}s "
      f"(analysis window {want_win:.3f}s, misalign {abs(span-want_win):.3f}s) "
      f"backends={npid} probes={probe_n:.0f} eff_rate={rate:.1f}Hz "
      f"cpu_n={cpu} wait_n={wait} idle_clientread_n={idle}")
print(f"PGWS    {label:10s} share={share:5.2f}%  cpu_s={cpu*w:8.2f} "
      f"nonidle_s={tot*w:8.2f} cpu_n={cpu} nonidle_n={tot}")
for k, v in per_ev.most_common(12):
    print(f"    {label:10s} {k:26s} {100.0*v/tot if tot else 0:6.2f}%  {v*w:8.2f}s")
