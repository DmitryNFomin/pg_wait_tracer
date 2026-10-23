import sys
for tag in sys.argv[1:]:
    ts = [float(x) for x in open(f"rounds-{tag}.txt") if x.strip()]
    gaps = sorted((b - a) * 1000 for a, b in zip(ts, ts[1:]))
    n = len(gaps); q = lambda p: gaps[min(n - 1, int(p * n))]
    span = (ts[-1] - ts[0])
    print(f"{tag:9} rounds={len(ts)} achieved={len(ts)/span:.1f}Hz gap p50={q(.5):.2f}ms p90={q(.9):.2f} "
          f"p99={q(.99):.2f} p99.9={q(.999):.2f} max={gaps[-1]:.2f}ms  >15ms={sum(g>15 for g in gaps)/n*100:.2f}% "
          f">20ms={sum(g>20 for g in gaps)/n*100:.2f}% >50ms={sum(g>50 for g in gaps)}")
