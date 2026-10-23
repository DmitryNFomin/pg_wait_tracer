# shtail.py DIR: from the patch rings, per request (segment ending at TxnCommit) server time and its waits.
# Compares "what dominates total wait time" (what a sampler ranks by) with "what dominates the slow requests".
import sys, csv, collections
d = sys.argv[1]; SLOW = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
rows = collections.defaultdict(list)
for r in csv.reader(open(f"{d}/rings.csv")):
    if r[1] == "req": rows[r[0]].append((int(r[3]), int(r[4]), r[5], r[6], float(r[7] or 0)))
total = collections.Counter(); slow_dom = collections.Counter(); slow_time = collections.Counter(); n = ns = 0
examples = []
for pid, rs in rows.items():
    rs.sort()
    seg = []; arrival = None
    for seq, ts, typ, ev, dur in rs:
        if typ == "Client" and ev == "ClientRead" and arrival is None:
            arrival = ts  # end of the idle wait before the request = request arrival
            continue
        if typ == "Query" and ev == "TxnCommit":
            if arrival is not None and seg is not None:
                srv = (ts - arrival) / 1e6
                w = collections.Counter()
                for _, _, t2, e2, du in seg:
                    if t2 == "Client": w["Client:in-transaction round trip"] += du / 1e3
                    elif t2 != "Query": w[f"{t2}:{e2}"] += du / 1e3
                n += 1
                for k, v in w.items(): total[k] += v
                if srv > SLOW:
                    ns += 1
                    dom = w.most_common(1)[0] if w else ("CPU/other", 0)
                    if dom[1] < 0.5 * srv: dom = ("CPU/other (no wait > 50%)", 0)
                    slow_dom[dom[0]] += 1
                    for k, v in w.items(): slow_time[k] += v
                    if len(examples) < 6: examples.append((round(srv, 1), [(k, round(v, 1)) for k, v in w.most_common(3)]))
            seg = []; arrival = None
            continue
        if arrival is not None: seg.append((seq, ts, typ, ev, dur))
T = sum(total.values()); S = sum(slow_time.values())
print(f"== {d}: requests reconstructed {n}; server time > {SLOW} ms: {ns} ({100*ns/max(1,n):.2f}%)")
print("  share of ALL request wait time (what a time-share profile ranks): " + ", ".join(f"{k} {100*v/T:.1f}%" for k, v in total.most_common(5)))
print("  dominant wait of each SLOW request: " + ", ".join(f"{k} {v} ({100*v/max(1,ns):.0f}%)" for k, v in slow_dom.most_common(5)))
print("  share of wait time inside slow requests: " + ", ".join(f"{k} {100*v/max(1e-9,S):.1f}%" for k, v in slow_time.most_common(4)))
for e in examples: print(f"    e.g. {e[0]} ms: {e[1]}")
