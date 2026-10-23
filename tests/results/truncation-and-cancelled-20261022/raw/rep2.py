# rep2.py DIR...: cancelled lookups: client outcome vs sampler vs pgss vs pg_stat_io vs tracer per-execution reads
import sys, os, csv, json, subprocess, statistics, collections, gzip
def med(a): return statistics.median(a) if a else float("nan")
for d in sys.argv[1:]:
    t0, t1 = float(open(f"{d}/t0").read()), float(open(f"{d}/t1").read())
    req = [l.split() for l in (gzip.open(f"{d}/req-all.txt.gz", "rt") if os.path.exists(f"{d}/req-all.txt.gz") else open(f"{d}/req-all.txt"))]
    win = [r for r in req if t0 <= float(r[1]) < t1]
    to = [r for r in win if r[4] == "timeout"]; ok = [r for r in win if r[4] == "ok"]
    okl = sorted((float(r[1]) - float(r[0])) * 1000 for r in ok)
    tol = sorted((float(r[1]) - float(r[0])) * 1000 for r in to)
    print(f"=== {d}: window requests {len(win)}, timeouts {len(to)} ({100*len(to)/len(win):.2f}%); ok p50 {med(okl):.2f} ms; cancelled requests took p50 {med(tol):.1f} / max {tol[-1] if tol else 0:.1f} ms")
    ev = collections.Counter()
    for et, e, c in csv.reader(open(f"{d}/pgws.csv")):
        if e != "ClientRead": ev[f"{et}:{e}"] += int(c)
    S = sum(ev.values())
    print("  pg_wait_sampling (client backends): " + ", ".join(f"{k} {100*v/S:.1f}%" for k, v in ev.most_common(3)))
    calls = br = rt = 0
    for r in csv.reader(open(f"{d}/pgss.csv")):
        calls += int(r[1]); br += int(r[3]); rt += float(r[4])
    print(f"  pg_stat_statements: calls {calls} (requests in window ~{len(win)}, timeouts not counted); blocks read/call {br/max(1,calls):.1f}; read time/call {rt/max(1,calls):.2f} ms")
    for r in csv.reader(open(f"{d}/pgio.csv")): print(f"  pg_stat_io: reads {r[0]}, avg {float(r[1])/max(1,int(r[0])):.3f} ms/read")
    b0 = {r[0]: (float(r[1]), float(r[2])) for r in csv.reader(open(f"{d}/bio0.csv"))}
    b1 = {r[0]: (float(r[1]), float(r[2])) for r in csv.reader(open(f"{d}/bio1.csv"))}
    for p in sorted(b1)[:2]:
        if p in b0:
            dr = b1[p][0] - b0[p][0]; dt = b1[p][1] - b0[p][1]
            print(f"  pg_stat_get_backend_io({p}): reads {dr:.0f}, avg {dt/max(1,dr):.3f} ms/read (cumulative for the whole backend, ok + cancelled)")
    if not os.path.isdir(f"{d}/trace"): continue
    srv = subprocess.Popen(["/root/pgwt-server", f"{d}/trace"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def ask(r):
        srv.stdin.write(json.dumps(r) + "\n"); srv.stdin.flush(); return json.loads(srv.stdout.readline())
    rows = ask({"id": 1, "cmd": "executions", "sort": "duration_desc", "limit": 20000})["rows"]
    seen = set(); slow = []; fast = []
    for x in rows:
        k = (x["pid"], x["start_ns"])
        if k in seen or x.get("duration_ms") is None: continue
        seen.add(k); (slow if x["duration_ms"] >= 10 else fast).append(x)
    def reads(x):
        s, e = int(x["start_ns"]), int(x["end_ns"])
        det = ask({"id": 2, "cmd": "execution_detail", "start_ns": x["start_ns"], "end_ns": x["end_ns"], "filters": {"pid": x["pid"]}})
        n = 0; tot = 0
        for ev in det.get("leader", {}).get("events", []):
            if ev["name"] == "IO:DataFileRead":
                a = int(ev["start_ns"]); b = a + int(ev["dur_ns"]); a, b = max(a, s), min(b, e)
                if b > a: n += 1; tot += (b - a) / 1e3
        return n, tot
    res = [reads(x) for x in slow[:1500]]
    rn = [r[0] for r in res]; ra = [r[1] / r[0] for r in res if r[0]]
    print(f"  pg_wait_tracer: executions >= 10 ms (timed out or about to): {len(slow)}; per execution: reads median {med(rn):.0f} (p10 {sorted(rn)[len(rn)//10]}, p90 {sorted(rn)[9*len(rn)//10]}), avg read median {med(ra):.0f} us")
    res2 = [reads(x) for x in fast[-500:]]
    print(f"  pg_wait_tracer: fast executions (sample): reads median {med([r[0] for r in res2]):.0f}, avg read median {med([r[1]/r[0] for r in res2 if r[0]]):.0f} us")
    srv.stdin.close()
