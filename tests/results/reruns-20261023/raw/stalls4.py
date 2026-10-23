# stalls4.py DIR...: for slow requests outside stall intervals (K=20 concurrent slow requests), distance to the nearest
# stall interval, and whether they cluster in time (per 100 ms bin).
import sys, os, gzip, collections
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
K = 20
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    slow = []
    for l in opn(f"{d}/lat-all.txt"):
        f = l.split(); e = int(f[4]) + int(f[5]) / 1e6; lat = int(f[2]) / 1e6
        if t0 <= e < t1 and lat > 0.005: slow.append((e - lat + 0.005, e, lat))
    ev = sorted([(s, 1) for s, e, _ in slow] + [(e, -1) for s, e, _ in slow]); cur = 0; ivs = []; st = None
    for t, x in ev:
        cur += x
        if cur >= K and st is None: st = t
        if cur < K and st is not None: ivs.append((st, t)); st = None
    kept = [r for r in slow if not any(a < r[1] and r[0] < b for a, b in ivs)]
    dist = collections.Counter()
    for s, e, _ in kept:
        dd = min([abs(s - b) for a, b in ivs] + [abs(e - a) for a, b in ivs], default=99)
        dist["<0.5s" if dd < .5 else "<2s" if dd < 2 else ">=2s"] += 1
    bins = collections.Counter(int(s * 10) for s, e, _ in kept)
    print(f"{d.split('/')[-1]}: kept {len(kept)}; distance to a stall {dict(dist)}; busiest 100 ms bin {max(bins.values(), default=0)}; "
          f"bins with >=10: {sum(v for v in bins.values() if v >= 10)} requests in {sum(1 for v in bins.values() if v >= 10)} bins")
