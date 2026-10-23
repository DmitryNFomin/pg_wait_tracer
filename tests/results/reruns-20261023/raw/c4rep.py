# c4rep.py DIR...: case 4 on the patched build. Client outcome, sampler, pgss, pg_stat_io, patch stats (delta over the
# window) and per-execution reads reconstructed from the trace rings (executions that started inside the window).
import sys, os, csv, gzip, collections, statistics
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else (open(p) if os.path.exists(p) else None)
def q(v, p): return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")
B = [2 ** i for i in range(32)]
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    wall0, mono0 = map(float, open(f"{d}/clock0").read().split())
    m0, m1 = (t0 - wall0) * 1e9 + mono0, (t1 - wall0) * 1e9 + mono0
    req = [l.split() for l in opn(f"{d}/req-all.txt")]
    win = [r for r in req if t0 <= float(r[0]) < t1]
    lat = sorted((float(r[1]) - float(r[0])) * 1000 for r in win)
    to = sum(1 for r in win if r[4] == "timeout")
    print(f"=== {d}: window {t1-t0:.1f} s, requests {len(win)} ({len(win)/(t1-t0):.0f}/s), timeouts {to} ({100*to/max(len(win),1):.2f}%), "
          f"latency p50 {q(lat,.5):.2f} p99 {q(lat,.99):.2f} max {lat[-1]:.1f} ms")
    pw = [(a, b, int(c)) for a, b, c in csv.reader(open(f"{d}/pgws.csv"))]
    S = sum(c for a, b, c in pw if b != "ClientRead")
    print("  pg_wait_sampling (client backends, excl. ClientRead): " + ", ".join([f"{a}:{b} {100*c/S:.1f}%" for a, b, c in pw if b != "ClientRead"][:4]))
    for r in csv.reader(open(f"{d}/pgss.csv")):
        if int(r[1]) > 1000: print(f"  pgss: calls {r[1]}, blocks read/call {int(r[3])/int(r[1]):.1f}, read time/call {float(r[4])/int(r[1]):.2f} ms, mean {r[5]} ms, max {r[6]} ms")
    for r in csv.reader(open(f"{d}/pgio.csv")): print(f"  pg_stat_io: reads {r[0]}, avg {float(r[1])/int(r[0]):.3f} ms")
    def tim(f):
        out = {}
        for r in csv.reader(open(f)):
            out[(r[0], r[3], r[4])] = (int(r[5]), float(r[6]), [int(x) for x in r[9].strip("{}").split(",")])
        return out
    a0, a1 = tim(f"{d}/timing0.csv"), tim(f"{d}/timing1.csv")
    if a1:
        agg = collections.defaultdict(lambda: [0, 0.0, [0] * 32])
        for k, (c, ms, h) in a1.items():
            c0, ms0, h0 = a0.get(k, (0, 0.0, [0] * 32))
            g = agg[(k[1], k[2])]; g[0] += c - c0; g[1] += ms - ms0; g[2] = [x + y - z for x, y, z in zip(g[2], h, h0)]
        tot = sum(v[1] for k, v in agg.items() if k[1] != "ClientRead")
        print("  patch stats (window delta, client backends): " + ", ".join([
            f"{k[0]}:{k[1]} {v[0]} calls, {v[1]:.0f} ms, avg {1000*v[1]/max(v[0],1):.0f} us" for k, v in sorted(agg.items(), key=lambda x: -x[1][1]) if k[1] != "ClientRead"][:3]))
        h = agg[("IO", "DataFileRead")][2]
        print("  patch DataFileRead histogram (bucket upper bound us: share): " + ", ".join(f"<{B[i]}:{100*x/max(sum(h),1):.0f}%" for i, x in enumerate(h) if x and 100*x/sum(h) >= 1))
    f = opn(f"{d}/rings.csv")
    if not f: continue
    # per pid, ordered by seq: an execution runs from ExecStart (or QueryStart) to TxnCommit / TxnAbort
    ex = []; cur = {}; gaps = collections.Counter(); last = {}
    for r in csv.reader(f):
        pid, seq, ts, typ, ev, dur = r[0], int(r[2]), int(r[3]), r[4], r[5], float(r[6])
        if pid in last and seq != last[pid] + 1: gaps[pid] += seq - last[pid] - 1
        last[pid] = seq
        if typ == "Query":
            if ev in ("ExecStart", "QueryStart") and pid not in cur: cur[pid] = [ts, 0, 0.0, None]
            elif ev in ("TxnCommit", "TxnAbort") and pid in cur:
                e = cur.pop(pid); e[3] = ev; e.append(ts); ex.append(e)
            elif ev == "Idle": cur.pop(pid, None)
        elif pid in cur and (typ, ev) == ("IO", "DataFileRead"):
            cur[pid][1] += 1; cur[pid][2] += dur
    ex = [e for e in ex if m0 <= e[0] < m1]
    print(f"  ring: executions in window {len(ex)} (records lost to ring wrap: {sum(gaps.values())})")
    def summ(name, xs):
        if not xs: print(f"    {name}: none"); return
        n = sum(e[1] for e in xs); t = sum(e[2] for e in xs)
        dur = sorted((e[4] - e[0]) / 1e6 for e in xs)
        print(f"    {name}: {len(xs)} executions, reads/execution {n/len(xs):.1f} (median {statistics.median(e[1] for e in xs):.0f}), "
              f"avg read {t/max(n,1):.0f} us, read time share of execution {100*t/1000/max(sum(dur),1e-9):.0f}%, exec p50 {q(dur,.5):.2f} ms")
    summ("all", ex)
    summ("completed", [e for e in ex if e[3] == "TxnCommit"])
    summ("cancelled (TxnAbort)", [e for e in ex if e[3] == "TxnAbort"])
    summ("slowest 500", sorted(ex, key=lambda e: e[0] - e[4])[:500])
