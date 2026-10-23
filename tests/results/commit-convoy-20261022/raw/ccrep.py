# ccrep.py DIR...: latency spikes vs fio bursts; sampler profile overall and inside bursts
import sys, os, csv, collections
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    bursts = [tuple(map(float, l.split())) for l in open(f"{d}/bursts.txt")] if os.path.exists(f"{d}/bursts.txt") else []
    inb = lambda t: any(a <= t <= b + 0.5 for a, b in bursts)
    lat = []
    for l in open(f"{d}/lat-all.txt"):
        f = l.split(); end = int(f[4]) + int(f[5]) / 1e6
        if t0 <= end < t1: lat.append((end, int(f[2]) / 1000, int(f[6]) / 1000 if len(f) > 6 else 0))
    s = sorted(x[1] for x in lat); n = len(s); q = lambda p: s[min(n - 1, int(p * n))]
    slow = [x for x in lat if x[1] > 50]
    print(f"=== {d}: txns {n}; latency p50 {q(.5):.1f} p99 {q(.99):.1f} p99.9 {q(.999):.1f} max {s[-1]:.0f} ms; > 50 ms: {len(slow)}, inside bursts(+0.5s): {sum(1 for x in slow if inb(x[0]))}; bursts {len(bursts)}")
    pids_req = {r[0] for r in csv.reader(open(f"{d}/activity.csv")) if r[2] == "req"}
    ev = collections.Counter()
    for pid, et, e, c in csv.reader(open(f"{d}/pgws-all.csv")):
        if pid in pids_req and e != "ClientRead": ev[f"{et}:{e}"] += int(c)
    S = sum(ev.values()) or 1
    print("  pg_wait_sampling profile (client backends): " + ", ".join(f"{k} {100*v/S:.1f}%" for k, v in ev.most_common(6)))
    if os.path.exists(f"{d}/pgws-hist-poll.csv") and bursts:
        seen = set(); evb = collections.Counter(); evo = collections.Counter()
        for r in csv.reader(open(f"{d}/pgws-hist-poll.csv")):
            if len(r) < 5 or (r[0], r[1]) in seen: continue
            seen.add((r[0], r[1]))
            if r[3] == "ClientRead": continue
            (evb if inb(float(r[0])) else evo)[f"{r[2]}:{r[3]}"] += 1
        B = sum(evb.values()) or 1; O = sum(evo.values()) or 1
        print(f"  sampler history (polled, {len(seen)} rows) inside bursts: " + ", ".join(f"{k} {100*v/B:.0f}%" for k, v in evb.most_common(5)))
        print(f"  sampler history outside bursts: " + ", ".join(f"{k} {100*v/O:.0f}%" for k, v in evo.most_common(5)))
