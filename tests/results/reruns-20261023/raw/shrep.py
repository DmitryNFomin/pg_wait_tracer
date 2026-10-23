# shrep.py DIR...: request latency (window), sampler per-query profile for the request statements
import sys, os, csv, collections
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    lat = []
    for l in open(f"{d}/lat-all.txt"):
        f = l.split(); end = int(f[4]) + int(f[5]) / 1e6
        if t0 <= end < t1: lat.append(int(f[2]) / 1000)
    lat.sort(); n = len(lat); q = lambda p: lat[min(n - 1, int(p * n))]
    print(f"=== {d} (shared_buffers {open(f'{d}/shared_buffers.txt').read().strip()}): requests {n}; mean {sum(lat)/n:.2f} p50 {q(.5):.2f} p99 {q(.99):.2f} p99.9 {q(.999):.2f} max {lat[-1]:.1f} ms; > 5 ms: {sum(1 for x in lat if x > 5)}")
    tot = collections.Counter(); per = collections.defaultdict(collections.Counter)
    for qt, et, e, c in csv.reader(open(f"{d}/pgws-by-query.csv")):
        if e == "ClientRead": continue
        tot[f"{et}:{e}"] += int(c); per[qt[:22]][f"{et}:{e}"] += int(c)
    S = sum(tot.values())
    print("  pg_wait_sampling, request backends: " + ", ".join(f"{k} {100*v/S:.1f}%" for k, v in tot.most_common(6)))
    for qt, c in per.items():
        T = sum(c.values())
        if T > 0.05 * S: print(f"    {qt:22s} {100*T/S:4.0f}% of samples: " + ", ".join(f"{k} {100*v/T:.0f}%" for k, v in c.most_common(4)))
    for r in csv.reader(open(f"{d}/pgss.csv")): print(f"  pgss: {r[0][:40]:40s} calls {r[1]} mean {r[3]} ms max {r[4]} ms blks_read {r[5]}")
