# rep1.py DIR...: truncation convoy summary
import sys, os, csv, collections, gzip
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
for d in sys.argv[1:]:
    t0 = float(open(f"{d}/t0").read())
    ph = collections.defaultdict(list)
    for l in open(f"{d}/progress.txt"):
        f = l.split()
        if len(f) >= 3 and f[0][0].isdigit(): ph[" ".join(f[2:-3])].append(float(f[0]))
    tr = ph.get("truncating heap", [])
    lat = []
    for l in opn(f"{d}/lat-all.txt"):
        f = l.split(); end = int(f[4]) + int(f[5]) / 1e6; L = int(f[2]) / 1000; lag = int(f[6]) / 1000 if len(f) > 6 else 0
        lat.append((end, L, L - lag))
    n = len(lat); s = sorted(x[1] for x in lat); svc = sorted(x[2] for x in lat)
    q = lambda a, p: a[min(len(a) - 1, int(p * len(a)))]
    print(f"=== {d}: requests {n}; latency p50 {q(s,.5):.2f} p99 {q(s,.99):.2f} p99.9 {q(s,.999):.2f} p99.99 {q(s,.9999):.2f} max {s[-1]:.1f} ms; service-time p99.9 {q(svc,.999):.2f} max {svc[-1]:.1f}")
    for k, v in ph.items(): print(f"  vacuum phase {k:20s} {min(v)-t0:7.1f} .. {max(v)-t0:7.1f} s")
    slow = [x for x in lat if x[1] > 15]
    if tr:
        a, b = min(tr) - 0.5, max(tr) + 0.5
        ins = [x for x in slow if a <= x[0] - x[1] / 1000 <= b or a <= x[0] <= b]
        print(f"  requests > 15 ms: {len(slow)} ({100*len(slow)/n:.3f}%); inside truncation window ±0.5 s: {len(ins)}")
    else:
        print(f"  requests > 15 ms: {len(slow)} ({100*len(slow)/n:.3f}%); no truncation phase seen")
    # per-second timeline of slow requests
    sec = collections.Counter(int(x[0] - t0) for x in slow)
    print("  slow requests per second (t: count): " + ", ".join(f"{k}:{v}" for k, v in sorted(sec.items()) if v >= 3))
    if os.path.getsize(f"{d}/pgws-by-query.csv") > 0:
        bq = collections.defaultdict(collections.Counter)
        for qt, et, e, c in csv.reader(open(f"{d}/pgws-by-query.csv")):
            if e != "ClientRead": bq[qt][f"{et}:{e}"] += int(c)
        for qt, c in bq.items():
            if qt.startswith("SELECT payload"):
                T = sum(c.values()); print(f"  pg_wait_sampling, lookup ({T} samples): " + ", ".join(f"{k} {100*v/T:.1f}%" for k, v in c.most_common(5)) + f"; Lock:relation {100*c['Lock:relation']/T:.2f}%")
        g = collections.Counter()
        for pid, et, e, c in csv.reader(open(f"{d}/pgws-all.csv")):
            if e not in ("ClientRead",) and et not in ("Activity",): g[f"{et}:{e}"] += int(c)
        T = sum(g.values()); print(f"  pg_wait_sampling, all backends: " + ", ".join(f"{k} {100*v/T:.1f}%" for k, v in g.most_common(5)) + f"; Lock:relation {100*g['Lock:relation']/T:.2f}%")
        h = list(csv.reader(open(f"{d}/pgws-history.csv")))
        if h: print(f"  pg_wait_sampling history ring at the end: {len(h)} rows, from {h[0][0][11:19]} to {h[-1][0][11:19]}; Lock:relation rows {sum(1 for r in h if r[3]=='relation')}")
    for r in csv.reader(open(f"{d}/pgss.csv")):
        if r[0].startswith("SELECT payload"): print(f"  pgss lookup: calls {r[1]} mean {r[3]} ms max {r[4]} ms")
    pl = open(f"{d}/pglog.txt", errors="replace").read()
    print(f"  server log: 'still waiting' {pl.count('still waiting')}, autovacuum lines {pl.count('automatic vacuum')}")
    if os.path.exists(f"{d}/blockers.txt"):
        rows = [l.split(None, 5) for l in open(f"{d}/blockers.txt") if l[:1].isdigit()]
        print(f"  pg_blocking_pids poller: {len(rows)} waiter rows; blockers: {dict(collections.Counter(r[4] if len(r)>4 else '?' for r in rows))}")
