#!/usr/bin/env python3
"""Summarise /root/ovh294.log: daemon CPU, base vs fix, per rate, with spread.

A cell with fewer than the requested n usable runs says so; discarded runs
(OVH-ERROR lines) are listed, never silently averaged away.
"""
import re, sys, statistics as st

path = sys.argv[1] if len(sys.argv) > 1 else "/root/ovh294.log"
txt = open(path).read()
errs = re.findall(r"^OVH-ERROR.*$", txt, re.M)
cells = {}
for m in re.finditer(r"^OVH (\w+) rate=(\d+) iter=(\d+) ticks=(\d+) "
                     r"wall_s=([\d.]+) cpu_ns=(\d+) pct_one_core=([\d.]+) "
                     r"samples=(\d+) at_risk=(\S+) recovered=(\S+) "
                     r"read_failed=(\S+)", txt, re.M):
    tree, rate = m.group(1), int(m.group(2))
    cells.setdefault((tree, rate), []).append({
        "ticks": int(m.group(4)), "wall": float(m.group(5)),
        "pct": float(m.group(7)), "samples": int(m.group(8)),
        "at_risk": m.group(9), "recovered": m.group(10),
        "read_failed": m.group(11)})

print("Daemon CPU, --mode sampled, 60 s steady-state window, pgbench -c8 -j2")
print("%5s %-5s %3s %9s %9s %9s %12s %12s" % (
    "rate", "tree", "n", "median%", "min%", "max%", "median_ticks", "samples"))
res = {}
for rate in sorted({r for _, r in cells}):
    for tree in ("base", "fix"):
        v = cells.get((tree, rate), [])
        if not v:
            print("%5d %-5s   0  NO USABLE RUNS" % (rate, tree)); continue
        p = [x["pct"] for x in v]
        res[(tree, rate)] = p
        print("%5d %-5s %3d %9.4f %9.4f %9.4f %12.0f %12.0f" % (
            rate, tree, len(p), st.median(p), min(p), max(p),
            st.median([x["ticks"] for x in v]),
            st.median([x["samples"] for x in v])))
print()
print("Delta (fix - base), median, and whether the spreads even separate:")
for rate in sorted({r for _, r in cells}):
    b, f = res.get(("base", rate)), res.get(("fix", rate))
    if not (b and f):
        print("  %d Hz: ABSENT" % rate); continue
    db = st.median(f) - st.median(b)
    overlap = not (max(b) < min(f) or max(f) < min(b))
    print("  %4d Hz: base %.4f%% -> fix %.4f%% of one core, delta %+.4f pp "
          "(%+.1f%% relative); ranges %s  [base %.4f-%.4f, fix %.4f-%.4f]" % (
          rate, st.median(b), st.median(f), db,
          100.0 * db / st.median(b) if st.median(b) else float('nan'),
          "OVERLAP -> not separable at this n" if overlap else "disjoint",
          min(b), max(b), min(f), max(f)))
print()
for rate in sorted({r for _, r in cells}):
    v = cells.get(("fix", rate), [])
    if v:
        print("  fix @%d Hz recheck counters (last run): at_risk=%s "
              "recovered=%s read_failed=%s" % (
              rate, v[-1]["at_risk"], v[-1]["recovered"], v[-1]["read_failed"]))
if errs:
    print("\nDISCARDED RUNS (not averaged in):")
    for e in errs:
        print("  " + e)
else:
    print("\nNo discarded runs.")
