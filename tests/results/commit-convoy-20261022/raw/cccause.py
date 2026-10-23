# cccause.py DIR KIND: (1) per-call WAL flush / lock wait durations inside vs outside bursts (exact data)
# (2) for each long lock wait: did it end right after another backend's WAL flush (WalSync/WALWrite/WalWrite) ended?
import sys, re, csv, collections, statistics, bisect
d, kind = sys.argv[1], sys.argv[2]
w0, m0 = map(float, open(f"{d}/clock0").read().split()); tomono = lambda w: w - w0 + m0
bursts = [(tomono(a), tomono(b) + 0.5) for a, b in (map(float, l.split()) for l in open(f"{d}/bursts.txt"))]
t0, t1 = tomono(float(open(f"{d}/t0").read())), tomono(float(open(f"{d}/t1").read()))
inb = lambda t: any(a <= t <= b for a, b in bursts)
EV = ("IO:WalSync", "LWLock:WALWrite", "IO:WalWrite", "Lock:transactionid", "Lock:tuple")
iv = []
if kind == "tracer":
    rx = re.compile(r"ts=(\d+) pid=(\d+) old=0x[0-9a-f]+\(([^)]*)\) new=\S+ dur_ms=([\d.]+)")
    for l in open(f"{d}/raw.txt", errors="replace"):
        m = rx.match(l)
        if m and m.group(3) in EV:
            e = int(m.group(1)) / 1e9; iv.append((e - float(m.group(4)) / 1e3, e, m.group(2), m.group(3), float(m.group(4))))
else:
    for r in csv.reader(open(f"{d}/rings.csv")):
        ev = f"{r[5]}:{r[6]}"
        if ev in EV:
            e = int(r[4]) / 1e9; dur = float(r[7]) / 1e3; iv.append((e - dur / 1e3, e, r[0], ev, dur))
iv = [x for x in iv if t0 <= x[1] <= t1]
first_ts = min(x[1] for x in iv)
print(f"== {d} ({kind}), data from +{first_ts - t0:.0f} s of the window")
for ev in EV:
    for lab, sel in (("in bursts", True), ("outside", False)):
        ds = sorted(x[4] for x in iv if x[3] == ev and inb(x[1]) == sel)
        if ds: print(f"  {ev:20s} {lab:9s}: {len(ds):6d} waits, avg {statistics.mean(ds):6.2f} ms, p50 {ds[len(ds)//2]:6.2f}, p99 {ds[int(.99*len(ds))]:7.2f}, max {ds[-1]:7.1f}")
flush_end = sorted((x[1], x[2]) for x in iv if x[3] in ("IO:WalSync", "LWLock:WALWrite", "IO:WalWrite"))
fe = [f[0] for f in flush_end]
lk = [x for x in iv if x[3] == "Lock:transactionid" and x[4] >= 10]
hit = 0
for x in lk:
    i = bisect.bisect_right(fe, x[1]); ok = False
    j = i - 1
    while j >= 0 and fe[j] >= x[1] - 0.002:
        if flush_end[j][1] != x[2]: ok = True; break
        j -= 1
    hit += ok
print(f"  lock waits >= 10 ms: {len(lk)}; ended within 2 ms after another backend's WAL flush/write ended: {hit} ({100*hit/max(1,len(lk)):.0f}%)")
# control: same test at random end times
import random
rnd = [random.uniform(t0, t1) for _ in range(5000)]; rh = 0
for t in rnd:
    i = bisect.bisect_right(fe, t); rh += (i > 0 and fe[i - 1] >= t - 0.002)
print(f"  control: random instants with a WAL flush ending in the previous 2 ms: {100*rh/len(rnd):.0f}%")
