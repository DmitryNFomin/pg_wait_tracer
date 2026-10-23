# stalls3.py K DIR...: stall = a moment when at least K of the 16 clients are inside a request that is already
# over 5 ms old. A lock convoy only stalls the requests on the job's locked rows (100 of 1,000 items, ~10% of
# requests), so it should rarely put many clients in that state at once; a server-wide stall puts all of them there.
# A slow request (> 5 ms) is attributed to a stall if any part of its slow phase (start + 5 ms .. end) overlaps one.
import sys, os, gzip, collections
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
K = int(sys.argv[1])
for d in sys.argv[2:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    slow = []; hist = collections.Counter()
    for l in opn(f"{d}/lat-all.txt"):
        f = l.split(); e = int(f[4]) + int(f[5]) / 1e6; lat = int(f[2]) / 1e6
        if t0 <= e < t1 and lat > 0.005: slow.append((e - lat + 0.005, e, int(f[0])))
    ev = sorted([(s, 1) for s, e, c in slow] + [(e, -1) for s, e, c in slow])
    cur = 0; peak = 0; ivs = []; start = None
    for t, x in ev:
        cur += x; peak = max(peak, cur)
        if cur >= K and start is None: start = t
        if cur < K and start is not None: ivs.append((start, t)); start = None
    # peak concurrency per slow request episode (connected components of overlapping slow phases)
    comps = []; end = -1
    for s, e, c in sorted(slow):
        if s > end: comps.append([0, 0]); 
        comps[-1][1] += 1; end = max(end, e)
    hit = [r for r in slow if any(a < r[1] and r[0] < b for a, b in ivs)]
    kept = len(slow) - len(hit)
    print(f"{d.split('/')[-1]:8s} slow {len(slow):4d} | peak concurrent slow clients {peak:2d} | stall intervals (>= {K}) {len(ivs):2d}, "
          f"{sum(b-a for a,b in ivs)*1000:5.0f} ms | slow outside stalls {kept:4d}")
