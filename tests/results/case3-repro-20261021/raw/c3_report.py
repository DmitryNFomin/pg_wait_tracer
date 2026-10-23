import csv, sys, re
for tag in sys.argv[1:]:
    d = f"c3-{tag}"
    W = float(open(f"{d}/t1").read()) - float(open(f"{d}/t0").read())
    rows = [(r[0], r[1], int(r[2])) for r in csv.reader(open(f"{d}/pgws.csv"))]
    act = [(t, e, n) for t, e, n in rows if not (t == "Client" and e == "ClientRead")]
    tot = sum(n for *_, n in act)
    lat = [int(x) / 1000 for x in open(f"{d}/lat_us.txt") if x.strip()]
    q = lambda p: lat[min(len(lat) - 1, int(p * len(lat)))]
    print(f"==== {tag}: lookups={len(lat)}  latency p50={q(.5):.2f} p99={q(.99):.2f} p99.9={q(.999):.2f} max={lat[-1]:.1f} ms")
    print(f"  pg_wait_sampling (8 clients, {W:.0f} s): AAS {tot*0.01/W:.2f}")
    for t, e, n in sorted(act, key=lambda x: -x[2])[:4]:
        print(f"     {t+':'+e:24s} {100*n/tot:5.1f}%")
    for r in csv.reader(open(f"{d}/pgss.csv")):
        calls, hit, rd, rt, mean, mx = r
        print(f"  pg_stat_statements: calls={calls} blks read/call={int(rd)/int(calls):.1f} hit/call={int(hit)/int(calls):.1f} "
              f"read_time/call={float(rt)/int(calls):.2f} ms mean={mean} ms max={mx} ms")
    for r in csv.reader(open(f"{d}/pgio.csv")):
        print(f"  pg_stat_io: reads={r[0]} avg {float(r[1])/int(r[0]):.3f} ms/read")
