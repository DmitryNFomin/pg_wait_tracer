# bs12.py DIR...: wet-v12 by_statement() attribution check for the truncation convoy runs (capture = trace)
import sys, os, csv, collections, gzip
def opn(p): return gzip.open(p + ".gz", "rt") if os.path.exists(p + ".gz") else open(p)
for d in sys.argv[1:]:
    if not os.path.exists(f"{d}/by_statement.csv") and not os.path.exists(f"{d}/by_statement.csv.gz"): continue
    lk = collections.defaultdict(lambda: [0, 0.0]); rd = collections.defaultdict(int)
    for r in csv.reader(opn(f"{d}/by_statement.csv")):
        pid, bucket, sseq, qid, phase, typ, ev, calls, tot = r
        b = bucket if bucket.startswith("<") else "statement"
        if (typ, ev) == ("Lock", "relation"): lk[(b, phase)][0] += int(calls); lk[(b, phase)][1] += float(tot)
        if (typ, ev) == ("IO", "DataFileRead"): rd[(b, phase)] += int(calls)
    ring = [0, 0.0, 0.0]
    for r in csv.reader(opn(f"{d}/rings.csv")):
        if r[4] == "Lock" and r[5] == "relation":
            ring[0] += 1; ring[1] += float(r[6]); ring[2] = max(ring[2], float(r[6]))
    st = [0, 0.0]
    for r in csv.reader(open(f"{d}/timing.csv")):
        if r[3] == "Lock" and r[4] == "relation": st[0] += int(r[5]); st[1] += float(r[6])
    print(f"=== {d}: patch stats Lock:relation {st[0]} calls {st[1]:.0f} ms; ring {ring[0]} waits, avg {ring[1]/max(ring[0],1)/1000:.1f} ms, max {ring[2]/1000:.1f} ms")
    print("  by_statement Lock:relation: " + ", ".join(f"{b}/{p} {c} ({t/1000:.0f} ms)" for (b, p), (c, t) in sorted(lk.items())))
    print("  by_statement DataFileRead:  " + ", ".join(f"{b}/{p} {c}" for (b, p), c in sorted(rd.items())))
