import csv, re, os
IDLE = {("Client", "ClientRead"), ("Activity", "Activity")}
def sched(tag):
    a = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"sched0-{tag}.txt") if len(l.split()) >= 3}
    b = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"sched1-{tag}.txt") if len(l.split()) >= 3}
    ps = [p for p in a if p in b]
    return len(ps), sum(b[p][0]-a[p][0] for p in ps)/1e9, sum(b[p][1]-a[p][1] for p in ps)/1e9
for lim in ["none", "200", "100"]:
    for obs in ["pgws", "tracer"]:
        tag = f"{lim}-{obs}"
        if not os.path.exists(f"win-{tag}.txt"): continue
        t0, t1, th = open(f"win-{tag}.txt").read().split(); W = float(t1) - float(t0)
        rows = [(r[0], r[1], int(r[2])) for r in csv.reader(open(f"pgws-{tag}.csv"))]
        act = [(t, e, n) for t, e, n in rows if (t, e) not in IDLE and t != "Activity"]
        tot = sum(n for *_, n in act)
        rounds = len(open(f"rounds-{tag}.txt").read().split())
        n, on, rq = sched(tag)
        pgb = open(f"pgb-{tag}.txt").read()
        tps = re.search(r"tps = ([\d.]+)", pgb); lat = re.search(r"latency average = ([\d.]+) ms", pgb)
        print(f"=== limit={lim} observers={obs}  window {W:.1f}s  pgbench tps={tps.group(1) if tps else '?'} lat_avg={lat.group(1) if lat else '?'} ms")
        print(f"  kernel, {n} client backends: on-CPU {on/W:.2f}  run-queue/throttled {rq/W:.2f}  cgroup throttled {int(th)/1e6/W:.2f} (session-equivalents)")
        print(f"  pg_wait_sampling: collector {rounds/W:.1f} Hz; active sessions (samples x 10 ms / window) = {tot*0.01/W:.2f}")
        for t, e, k in sorted(act, key=lambda x: -x[2])[:6]:
            print(f"      {t+':'+e:28s} {100*k/tot:5.1f}%  AAS {k*0.01/W:.2f}")
        if obs == "tracer" and os.path.exists(f"tracer-{tag}.txt"):
            print("  pg_wait_tracer exact (all backends):")
            for l in open(f"tracer-{tag}.txt", errors="replace"):
                m = re.match(r"\s+(DB Time|CPU\*|Off-CPU\*|IO|Lock|LWLock|Client|IPC|Timeout|Extension)\s+([\d.]+)\s+([\d.]+)%", l)
                if m: print(f"      {m.group(1):12s} AAS {float(m.group(2))/1000/W:5.2f}  {m.group(3)}%")
