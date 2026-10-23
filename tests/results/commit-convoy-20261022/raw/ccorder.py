# ccorder.py DIR KIND(tracer|patch): per fio burst, which long wait starts first (tracer raw / patch rings),
# and what pg_wait_sampling's polled history shows for the same burst.
import sys, re, csv, collections
d, kind = sys.argv[1], sys.argv[2]
LONG = 20.0  # ms
w0, m0 = map(float, open(f"{d}/clock0").read().split())
tomono = lambda w: w - w0 + m0
bursts = [tuple(map(float, l.split())) for l in open(f"{d}/bursts.txt")]
iv = []  # (start_s, end_s, pid, event, dur_ms) for waits >= 1 ms
if kind == "tracer":
    rx = re.compile(r"ts=(\d+) pid=(\d+) old=0x[0-9a-f]+\(([^)]*)\) new=\S+ dur_ms=([\d.]+)")
    for l in open(f"{d}/raw.txt", errors="replace"):
        m = rx.match(l)
        if not m: continue
        dur = float(m.group(4))
        if dur < 1 or m.group(3) in ("Client:ClientRead", "CPU*") or m.group(3).startswith(("Activity", "Unknown")): continue
        e = int(m.group(1)) / 1e9; iv.append((e - dur / 1e3, e, m.group(2), m.group(3), dur))
else:
    for r in csv.reader(open(f"{d}/rings.csv")):
        if r[5] in ("Query", "Client", "Activity"): continue
        dur = float(r[7]) / 1e3
        if dur < 1: continue
        e = int(r[4]) / 1e9; iv.append((e - dur / 1e3, e, r[0], f"{r[5]}:{r[6]}", dur))
iv.sort()
hist = []
seen = set()
for r in csv.reader(open(f"{d}/pgws-hist-poll.csv")):
    if len(r) < 5 or (r[0], r[1]) in seen: continue
    seen.add((r[0], r[1]))
    if r[3] in ("ClientRead",) or r[2] == "Activity": continue
    hist.append((tomono(float(r[0])), r[1], f"{r[2]}:{r[3]}"))
hist.sort()
firstcount = collections.Counter(); leads = collections.defaultdict(list)
print(f"== {d} ({kind}): {len(iv)} waits >= 1 ms; {len(bursts)} bursts")
for a, b in bursts:
    A = tomono(a); W = [x for x in iv if A - 0.2 <= x[0] <= A + 3.5 and x[4] >= LONG]
    if not W: print(f"  burst @{a:.0f}: no long waits"); continue
    f = W[0]; firstcount[f[3]] += 1
    fe = {}
    for x in W:
        fe.setdefault(x[3], x)
    c = collections.Counter(x[3] for x in W)
    line = f"  burst +{A - tomono(bursts[0][0]):5.1f}s: first long wait {f[3]} pid {f[2]} at +{(f[0]-A)*1e3:6.1f} ms ({f[4]:.0f} ms)"
    for ev in ("IO:WalSync", "LWLock:WALWrite", "Lock:transactionid"):
        if ev in fe: line += f" | first {ev.split(':')[1]} +{(fe[ev][0]-A)*1e3:.1f}"; leads[ev].append((fe[ev][0] - f[0]) * 1e3)
    line += " | long waits: " + ", ".join(f"{k} {v}" for k, v in c.most_common(3))
    # sampler: first history sample of each event in the same window
    H = [h for h in hist if A - 0.2 <= h[0] <= A + 3.5]
    hf = {}
    for h in H: hf.setdefault(h[2], h[0])
    line += " || sampler first: " + ", ".join(f"{k.split(':')[1]} +{(v-A)*1e3:.0f}" for k, v in sorted(hf.items(), key=lambda kv: kv[1])[:3])
    print(line)
print("  first long wait per burst: " + ", ".join(f"{k} {v}" for k, v in firstcount.most_common()))
