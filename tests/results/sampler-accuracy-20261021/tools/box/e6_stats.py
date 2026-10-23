import csv, sys
bins = [(0.05, 0.5), (0.5, 1), (1, 2), (2, 5), (5, 10), (10, 20), (20, 1e9)]
for tag in sys.argv[1:]:
    rows = [(float(r[0]), r[1] == 't') for r in csv.reader(open(f"e6-{tag}.csv"))]
    print(f"== {tag}: {len(rows)} lock waits > 0.05 ms, seen by pg_wait_sampling: {sum(s for _, s in rows)}")
    for lo, hi in bins:
        b = [(m, s) for m, s in rows if lo <= m < hi]
        if not b: continue
        mean = sum(m for m, _ in b) / len(b)
        exp = sum(min(1, m / 10) for m, _ in b) / len(b)
        print(f"  {lo:>5}-{hi if hi < 1e8 else 'inf':>4} ms  n={len(b):5d}  seen={sum(s for _, s in b)/len(b)*100:5.1f}%"
              f"  expected min(1,d/10ms)={exp*100:5.1f}%  mean={mean:.2f} ms")
