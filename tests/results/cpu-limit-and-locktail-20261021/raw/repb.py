# repb.py DIR...: summary of runb.sh runs
import sys, os, csv, collections, gzip, statistics
def stat(f):
    d = {}
    for l in open(f):
        k, v = l.split(); d[k] = int(v)
    return d
for d in sys.argv[1:]:
    t0 = float(open(f"{d}/t0").read()); t1 = float(open(f"{d}/t1").read()); W = t1 - t0
    s0 = {l.split()[0]: list(map(int, l.split()[1:])) for l in open(f"{d}/sched0") if len(l.split()) == 4}
    s1 = {l.split()[0]: list(map(int, l.split()[1:])) for l in open(f"{d}/sched1") if len(l.split()) == 4}
    on = sum(s1[p][0] - s0[p][0] for p in s1 if p in s0) / 1e9 / W
    rq = sum(s1[p][1] - s0[p][1] for p in s1 if p in s0) / 1e9 / W
    c0, c1 = stat(f"{d}/cg0"), stat(f"{d}/cg1")
    use = (c1["usage_usec"] - c0["usage_usec"]) / 1e6 / W
    thr = (c1.get("throttled_usec", 0) - c0.get("throttled_usec", 0)) / 1e6 / W
    nper = c1.get("nr_periods", 0) - c0.get("nr_periods", 0); nthr = c1.get("nr_throttled", 0) - c0.get("nr_throttled", 0)
    # latency in window
    lat = []
    for l in (gzip.open(f"{d}/lat-all.txt.gz", "rt") if os.path.exists(f"{d}/lat-all.txt.gz") else open(f"{d}/lat-all.txt")):
        f = l.split()
        ts = int(f[4]) + int(f[5]) / 1e6
        if t0 <= ts < t1: lat.append(int(f[2]) / 1000)
    lat.sort(); n = len(lat)
    q = lambda p: lat[min(n - 1, int(p * n))]
    print(f"=== {d}  cpu.max {open(f'{d}/cpu.max').read().strip()}  window {W:.1f}s  txns {n} ({n/W:.0f}/s)")
    print(f"  latency ms: p50 {q(.5):.2f}  p90 {q(.9):.2f}  p99 {q(.99):.2f}  p99.9 {q(.999):.2f}  max {lat[-1]:.1f}")
    print(f"  cgroup: CPU used {use:.2f} cores; throttled in {nthr}/{nper} periods ({100*nthr/max(1,nper):.0f}%)")
    print(f"  kernel, {len(s1)} client backends: on-CPU {on:.2f}  waiting for a CPU (run-delay) {rq:.2f}  (session-equivalents)")
    pids = set(open(f"{d}/client-pids").read().split())
    ev = collections.Counter(); tot = collections.Counter()
    for pid, et, e, c in csv.reader(open(f"{d}/pgws-all.csv")):
        tot[pid] += int(c)
        if pid in pids and e != "ClientRead": ev[f"{et}:{e}"] += int(c)
    hz = statistics.mean(tot[p] for p in pids if p in tot) / W if pids else 0
    S = sum(ev.values())
    print(f"  pg_wait_sampling: {hz:.1f} samples/s per backend; active AAS (samples x 10 ms) {S*0.01/W:.2f}")
    for k, v in ev.most_common(7): print(f"      {k:28s} {100*v/S:5.1f}%  AAS {v*0.01/W:.2f}")
    tf = f"{d}/tracer-system_event.txt"
    if os.path.exists(tf):
        print("  pg_wait_tracer system_event (head):")
        for l in open(tf, errors="replace").read().splitlines()[:30]:
            if l.strip(): print("     " + l)
