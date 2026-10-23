# c5rep.py DIR...: summary of c5.sh runs (repb.py plus the wait-event timing patch's stats and per-transaction rings)
import sys, os, csv, collections, gzip, statistics
def stat(f):
    d = {}
    for l in open(f):
        k, v = l.split(); d[k] = int(v)
    return d
for d in sys.argv[1:]:
    t0 = float(open(f"{d}/t0").read()); t1 = float(open(f"{d}/t1").read()); W = t1 - t0
    s0 = {l.split()[0]: list(map(int, l.split()[1:])) for l in open(f"{d}/sched0") if len(l.split()) == 4}
    s1 = {l.split()[0]: list(map(int, l.split()[1:])) for l in open(f"{d}/sched1") if len(l.split()) == 4}
    on = sum(s1[p][0] - s0[p][0] for p in s1 if p in s0) / 1e9 / W
    rq = sum(s1[p][1] - s0[p][1] for p in s1 if p in s0) / 1e9 / W
    c0, c1 = stat(f"{d}/cg0"), stat(f"{d}/cg1")
    use = (c1["usage_usec"] - c0["usage_usec"]) / 1e6 / W
    thr = (c1.get("throttled_usec", 0) - c0.get("throttled_usec", 0)) / 1e6 / W
    nper = c1.get("nr_periods", 0) - c0.get("nr_periods", 0); nthr = c1.get("nr_throttled", 0) - c0.get("nr_throttled", 0)
    # latency in window
    lat = []
    for l in (gzip.open(f"{d}/lat-all.txt.gz", "rt") if os.path.exists(f"{d}/lat-all.txt.gz") else open(f"{d}/lat-all.txt")):
        f = l.split()
        ts = int(f[4]) + int(f[5]) / 1e6
        if t0 <= ts < t1: lat.append(int(f[2]) / 1000)
    lat.sort(); n = len(lat)
    q = lambda p: lat[min(n - 1, int(p * n))]
    print(f"=== {d}  cpu.max {open(f'{d}/cpu.max').read().strip()}  window {W:.1f}s  txns {n} ({n/W:.0f}/s)")
    print(f"  latency ms: p50 {q(.5):.2f}  p90 {q(.9):.2f}  p99 {q(.99):.2f}  p99.9 {q(.999):.2f}  max {lat[-1]:.1f}")
    print(f"  cgroup: CPU used {use:.2f} cores; throttled in {nthr}/{nper} periods ({100*nthr/max(1,nper):.0f}%)")
    print(f"  kernel, {len(s1)} client backends: on-CPU {on:.2f}  waiting for a CPU (run-delay) {rq:.2f}  (session-equivalents)")
    pids = set(open(f"{d}/client-pids").read().split())
    ev = collections.Counter(); tot = collections.Counter()
    for pid, et, e, c in csv.reader(open(f"{d}/pgws-all.csv")):
        tot[pid] += int(c)
        if pid in pids and e != "ClientRead": ev[f"{et}:{e}"] += int(c)
    hz = statistics.mean(tot[p] for p in pids if p in tot) / W if pids else 0
    S = sum(ev.values())
    print(f"  pg_wait_sampling: {hz:.1f} samples/s per backend; active AAS (samples x 10 ms) {S*0.01/W:.2f}")
    for k, v in ev.most_common(7): print(f"      {k:28s} {100*v/S:5.1f}%  AAS {v*0.01/W:.2f}")
    def tim(f):
        out = {}
        for r in csv.reader(open(f)):
            out[(r[0], r[3], r[4])] = (int(r[5]), float(r[6]), float(r[8]))
        return out
    if not os.path.exists(f"{d}/rings.csv") and not os.path.exists(f"{d}/rings.csv.gz"): continue
    a0, a1 = tim(f"{d}/timing0.csv"), tim(f"{d}/timing1.csv")
    agg = collections.defaultdict(lambda: [0, 0.0, 0.0])
    for k, (c, ms, mx) in a1.items():
        c0, ms0, _ = a0.get(k, (0, 0.0, 0.0))
        g = agg[f"{k[1]}:{k[2]}"]; g[0] += c - c0; g[1] += ms - ms0; g[2] = max(g[2], mx)
    act = sum(v[1] for k, v in agg.items() if not k.endswith("ClientRead"))
    print(f"  patch stats, client backends: waiting (excl. ClientRead) {act/1000/W:.2f} sessions; ClientRead {agg['Client:ClientRead'][1]/1000/W:.2f}")
    for k, v in sorted(agg.items(), key=lambda x: -x[1][1])[:7]:
        if k.endswith("ClientRead"): continue
        print(f"      {k:28s} {100*v[1]/act:5.1f}%  sessions {v[1]/1000/W:.2f}  calls {v[0]}  avg {1000*v[1]/max(v[0],1):.0f} us  max {v[2]/1000:.1f} ms")
    # per transaction from rings: BEGIN's ExecStart .. TxnCommit; share of the slowest 1% dominated by each wait event
    wall0, mono0 = map(float, open(f"{d}/clock0").read().split())
    m0, m1 = (t0 - wall0) * 1e9 + mono0, (t1 - wall0) * 1e9 + mono0
    f = gzip.open(f"{d}/rings.csv.gz", "rt") if os.path.exists(f"{d}/rings.csv.gz") else open(f"{d}/rings.csv")
    tx = []; cur = {}; lost = 0; last = {}
    for r in csv.reader(f):
        pid, seq, ts, typ, ev, dur = r[0], int(r[2]), int(r[3]), r[4], r[5], float(r[6])
        if pid in last and seq != last[pid] + 1: lost += seq - last[pid] - 1
        last[pid] = seq
        if typ == "Query":
            if ev in ("ExecStart", "QueryStart", "UtilityStart") and pid not in cur: cur[pid] = [ts, collections.Counter()]
            elif ev in ("TxnCommit", "TxnAbort") and pid in cur:
                e = cur.pop(pid); tx.append((e[0], ts, e[1]))
        elif pid in cur and ev != "ClientRead":
            cur[pid][1][f"{typ}:{ev}"] += dur
    tx = [t for t in tx if m0 <= t[0] < m1]
    tx.sort(key=lambda t: t[0] - t[1])
    top = tx[:max(1, len(tx) // 100)]
    dom = collections.Counter(); wt = collections.Counter(); el = 0.0
    for s, e, c in top:
        el += (e - s) / 1e3; wt.update(c)
        dom[c.most_common(1)[0][0] if c else "no wait"] += 1
    print(f"  patch rings: {len(tx)} transactions in window (records lost to ring wrap: {lost}); slowest 1% = {len(top)}, server time p50 {(top[len(top)//2][1]-top[len(top)//2][0])/1e6:.1f} ms")
    print("      dominant wait: " + ", ".join(f"{k} {100*v/len(top):.0f}%" for k, v in dom.most_common(4)))
    print(f"      wait share of their server time: {100*sum(wt.values())/el:.0f}%: " + ", ".join(f"{k} {100*v/el:.0f}%" for k, v in wt.most_common(4)))
