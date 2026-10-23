# repa.py DIR...: request latency in the window, sampler profile, statement stats, blocker evidence
import sys, os, csv, collections, gzip
for d in sys.argv[1:]:
    t0 = float(open(f"{d}/t0").read()); t1 = float(open(f"{d}/t1").read()); W = t1 - t0
    lat = []
    for l in (gzip.open(f"{d}/lat-all.txt.gz", "rt") if os.path.exists(f"{d}/lat-all.txt.gz") else open(f"{d}/lat-all.txt")):
        f = l.split(); ts = int(f[4]) + int(f[5]) / 1e6
        if t0 <= ts < t1: lat.append(int(f[2]) / 1000)
    lat.sort(); n = len(lat); q = lambda p: lat[min(n - 1, int(p * n))]
    slow = sum(1 for x in lat if x > 20)
    print(f"=== {d}  window {W:.1f}s  requests {n} ({n/W:.0f}/s)  deadlock_timeout {open(f'{d}/deadlock_timeout').read().strip()}")
    print(f"  request latency ms: p50 {q(.5):.2f} p99 {q(.99):.2f} p99.9 {q(.999):.2f} p99.99 {q(.9999):.2f} max {lat[-1]:.1f}; > 20 ms: {slow} ({100*slow/n:.3f}%)")
    if os.path.exists(f"{d}/bat-all.txt") and os.path.getsize(f"{d}/bat-all.txt"):
        b = [int(l.split()[2]) / 1000 for l in open(f"{d}/bat-all.txt")]
        print(f"  batch: {len(b)} runs, mean {sum(b)/len(b):.1f} ms")
    ev = collections.Counter()
    for et, e, c in csv.reader(open(f"{d}/pgws.csv")):
        if e != "ClientRead": ev[f"{et}:{e}"] += int(c)
    S = sum(ev.values())
    print("  pg_wait_sampling (request backends, excl. ClientRead): " + ", ".join(f"{k} {100*v/S:.1f}%" for k, v in ev.most_common(6)) + f"; Lock:transactionid {100*ev['Lock:transactionid']/S:.2f}%")
    bq = collections.defaultdict(collections.Counter)
    for qt, et, e, c in csv.reader(open(f"{d}/pgws-by-query.csv")):
        if e != "ClientRead": bq[qt][f"{et}:{e}"] += int(c)
    for qt, c in bq.items():
        if qt.startswith("UPDATE"):
            T = sum(c.values()); print(f"  per-query (UPDATE, {T} samples): " + ", ".join(f"{k} {100*v/T:.1f}%" for k, v in c.most_common(4)))
    for r in csv.reader(open(f"{d}/pgss.csv")): print(f"  pgss: {r[0][:45]:45s} calls {r[1]} mean {r[2]} ms max {r[3]} ms")
    if os.path.exists(f"{d}/blockers.txt"):
        rows = [l.split(None, 5) for l in open(f"{d}/blockers.txt") if l[:1].isdigit()]
        app = collections.Counter(r[4] if len(r) > 4 else "?" for r in rows)
        print(f"  pg_blocking_pids poller (5 ms): {len(rows)} waiter rows, blockers by application: {dict(app)}")
    print(f"  log lines 'still waiting for': {sum(1 for l in open(f'{d}/pglog.txt', errors='replace') if 'still waiting for' in l)}")
