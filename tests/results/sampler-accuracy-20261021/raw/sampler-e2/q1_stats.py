import csv, sys
for q in sys.argv[1:]:
    pids = set(open(f"q-pids-{q}.txt").read().strip().split(","))
    t0, t1 = map(float, open(f"q-win-{q}.txt").read().split()); W = t1 - t0
    rows = list(csv.reader(open(f"q-hist-{q}.csv")))
    rounds = sorted({float(r[0]) for r in rows})
    gaps = sorted((b - a) * 1000 for a, b in zip(rounds, rounds[1:]))
    vic = [r for r in rows if r[1] in pids]
    cpu = sum(r[2] == "CPU" for r in vic)
    s0 = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"q-sched0-{q}.txt")}
    s1 = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"q-sched1-{q}.txt")}
    on = sum(s1[p][0] - s0[p][0] for p in s0) / 1e9; rq = sum(s1[p][1] - s0[p][1] for p in s0) / 1e9
    n = len(gaps); g = lambda p: gaps[min(n - 1, int(p * n))]
    print(f"quota={q:5} window={W:.1f}s rounds={len(rounds)} ({len(rounds)/W:.1f} Hz) gap p50={g(.5):.1f} p99={g(.99):.1f} max={gaps[-1]:.1f} ms")
    print(f"   truth: 4 victims busy {4*W:.0f} session-s (wait_event 0 all the time); kernel on-CPU {on:.1f}s, run-queue {rq:.1f}s, other {4*W-on-rq:.1f}s")
    print(f"   pg_wait_sampling: victim samples={len(vic)} (CPU {cpu}); nominal DB time = samples x 10 ms = {len(vic)*0.01:.1f} s "
          f"= {100*len(vic)*0.01/(4*W):.0f}% of truth; AAS nominal {len(vic)*0.01/W:.2f} vs true 4.00")
