# stalls2.py DIR...: stall = a 100 ms bin (by request end time) where most requests were slow: median latency > 5 ms.
# A lock convoy on the restock job's 100 of 1,000 items touches ~10% of requests, so it cannot move a bin's median.
# Requests over 5 ms are then counted outside stalls: a request is excluded if its [start, end] overlaps a stall bin.
import sys, os, gzip, collections
BIN = 0.1
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    reqs = []
    for l in opn(f"{d}/lat-all.txt"):
        f = l.split(); e = int(f[4]) + int(f[5]) / 1e6; lat = int(f[2]) / 1e6
        if t0 <= e < t1: reqs.append((e - lat, e, lat * 1000, int(f[0])))
    bins = collections.defaultdict(list)
    for s, e, ms, c in reqs: bins[int(e / BIN)].append((ms, c))
    stall = set()
    for b, v in bins.items():
        ls = sorted(x for x, _ in v)
        if len(ls) >= 10 and ls[len(ls) // 2] > 5: stall.add(b)
    def in_stall(s, e): return any(b in stall for b in range(int(s / BIN), int(e / BIN) + 1))
    slow = [r for r in reqs if r[2] > 5]
    kept = [r for r in slow if not in_stall(r[0], r[1])]
    # stall episodes: merge adjacent bins
    eps = []; 
    for b in sorted(stall):
        if eps and b == eps[-1][1] + 1: eps[-1][1] = b
        else: eps.append([b, b])
    lat = sorted(r[2] for r in reqs); n = len(lat)
    latk = sorted(r[2] for r in reqs if not in_stall(r[0], r[1]))
    q = lambda v, p: v[min(len(v) - 1, int(p * len(v)))]
    print(f"{d}: requests {n}, > 5 ms {len(slow)}; stall episodes {len(eps)} "
          f"(total {sum(b-a+1 for a,b in eps)*BIN:.1f} s, longest {max([(b-a+1)*BIN for a,b in eps], default=0):.1f} s); "
          f"> 5 ms outside stalls {len(kept)}; p99.9 all {q(lat,.999):.2f} / outside stalls {q(latk,.999):.2f} ms; excluded requests {n-len(latk)}")
