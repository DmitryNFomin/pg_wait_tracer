# trtail.py DIR COMMIT_QID [SLOW_MS]: per-request reconstruction from pg_wait_tracer raw transitions (dump_raw),
# same method as shtail.py: a request runs from the end of the idle ClientRead that began after the previous COMMIT
# to the start of the ClientRead after this COMMIT. In-transaction ClientRead = client round trip.
import sys, re, collections
d, cq = sys.argv[1], sys.argv[2]; SLOW = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
pids = set(open(f"{d}/req-pids").read().split()) if False else None
rx = re.compile(r"ts=(\d+) pid=(\d+) old=0x[0-9a-f]+\(([^)]*)\) new=0x[0-9a-f]+\(([^)]*)\) dur_ms=([\d.]+) cpu_ns=(\d+) qid=(-?\d+)")
st = {}  # pid -> [arrival_ts or None, Counter waits]
total = collections.Counter(); slow_dom = collections.Counter(); slow_time = collections.Counter(); n = ns = 0; ex = []
for l in open(f"{d}/raw.txt", errors="replace"):
    m = rx.match(l)
    if not m: continue
    ts, pid, old, new, dur, cpu, qid = int(m.group(1)), m.group(2), m.group(3), m.group(4), float(m.group(5)), m.group(6), m.group(7)
    s = st.setdefault(pid, [None, collections.Counter()])
    if old == "Client:ClientRead" and qid == cq:      # idle wait after COMMIT ended -> request arrives
        s[0] = ts; s[1] = collections.Counter(); continue
    if s[0] is None or old.startswith("Unknown"): continue
    if old == "Client:ClientRead": s[1]["Client:in-transaction round trip"] += dur
    elif old != "CPU*": s[1][old] += dur
    if new == "Client:ClientRead" and qid == cq:      # COMMIT finished -> request done
        srv = (ts - s[0]) / 1e6; w = s[1]; n += 1
        for k, v in w.items(): total[k] += v
        if srv > SLOW:
            ns += 1
            dom = w.most_common(1)[0] if w else ("CPU/other", 0)
            if dom[1] < 0.5 * srv: dom = ("CPU/other (no wait > 50%)", 0)
            slow_dom[dom[0]] += 1
            for k, v in w.items(): slow_time[k] += v
            if len(ex) < 5: ex.append((round(srv, 1), [(k, round(v, 1)) for k, v in w.most_common(3)]))
        s[0] = None
T = sum(total.values()); S = sum(slow_time.values())
print(f"== {d} (pg_wait_tracer): requests reconstructed {n}; server time > {SLOW} ms: {ns} ({100*ns/max(1,n):.2f}%)")
print("  share of ALL request wait time: " + ", ".join(f"{k} {100*v/T:.1f}%" for k, v in total.most_common(5)))
print("  dominant wait of each SLOW request: " + ", ".join(f"{k} {v} ({100*v/max(1,ns):.0f}%)" for k, v in slow_dom.most_common(5)))
print("  share of wait time inside slow requests: " + ", ".join(f"{k} {100*v/max(1e-9,S):.1f}%" for k, v in slow_time.most_common(4)))
for e in ex: print(f"    e.g. {e[0]} ms: {e[1]}")
