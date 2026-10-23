# stalls.py DIR...: per-second view of the request log (pgbench -l: client, txn, latency_us, script, end_epoch, end_us).
# A stall second is one where the slowdown is broad: p50 of the second > 2 ms (normal p50 ~0.5 ms), or >= 12 of 16
# clients had a request over 5 ms. A lock convoy on the restock job's 100 rows touches ~10% of requests.
import sys, os, gzip, collections
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    sec = collections.defaultdict(list)
    for l in opn(f"{d}/lat-all.txt"):
        f = l.split(); e = int(f[4]) + int(f[5]) / 1e6
        if t0 <= e < t1: sec[int(e)].append((int(f[0]), int(f[2]) / 1000))
    rows = []
    for s, v in sec.items():
        lat = sorted(x for _, x in v); slow = [c for c, x in v if x > 5]
        rows.append((s, len(v), len(slow), lat[len(lat) // 2], len(set(slow)), len(slow) / len(v)))
    tot = sum(r[2] for r in rows)
    A = [r for r in rows if r[3] > 2]                  # p50 of the second > 2 ms
    Bc = [r for r in rows if r[4] >= 12]               # >= 12 of 16 clients slow
    C = [r for r in rows if r[2] >= 20]                # old count rule
    def out(sel): return tot - sum(r[2] for r in sel), len(sel)
    worst_nonstall = max((r for r in rows if r[3] <= 2 and r[4] < 12), key=lambda r: r[2], default=None)
    print(f"{d}: slow {tot} | outside p50>2ms seconds: {out(A)[0]} ({out(A)[1]} s) | outside >=12-client seconds: {out(Bc)[0]} ({out(Bc)[1]} s)"
          f" | outside old count rule: {out(C)[0]} ({out(C)[1]} s)")
    if worst_nonstall: s,n,k,p50,cl,fr = worst_nonstall; print(f"   busiest kept second: {k} slow of {n}, {cl} clients, p50 {p50:.2f} ms")
    for s,n,k,p50,cl,fr in sorted(A + [r for r in Bc if r not in A]): print(f"   stall {s}: {k}/{n} slow ({100*fr:.0f}%), {cl} clients, p50 {p50:.1f} ms")
