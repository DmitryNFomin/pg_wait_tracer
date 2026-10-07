"""Three checkable claims about frame_spacing, computed from the retained
summary.json corpus only.

C3 cliff fit: predicted_f3 = max(0, L3 - 1000) + e, L3 = max(500, A+s1) + s2,
   with A = achieved[0], s_k = capture_ms[k-1].  Reports the residual
   distribution over every tick of every tab of every run that recorded
   capture_ms.  A small residual means f3 is a step function of recorded
   capture cost -- an arithmetic consequence, not an independent measurement.

C4 separation: the max f3 over HEALTHY runs vs the f3 the deliberately
   regressed ("bypass") runs produced.  If healthy's max >= bypass's min, a
   single 150 ms bound cannot separate them.

C6 f4/f5: the same drift at the two frames whose 500 ms target gaps are wide
   enough not to clamp -- the honest read on paint cost.
"""
import json
import os
import statistics
import sys

BYPASS = {"1790798813", "1790798974"}   # scale="css" removed on purpose
# Pre-09-29 runs predate the mount-anchor fix (#193 r2) and show f4/f5 drift of
# 2400-2916 ms; they are a different instrument and are excluded by run.id.
ANCHOR_FIX_EPOCH = 1790700000


def rows(corpus):
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p):
            continue
        with open(p) as f:
            d = json.load(f)
        for name, tab in (d.get("tabs") or {}).items():
            for i, t in enumerate((tab.get("blink_sweep") or {}).get("ticks") or [], 1):
                a = t.get("achieved_offsets_ms") or []
                tg = t.get("target_offsets_ms") or []
                cm = t.get("capture_ms") or []
                if len(a) < 5 or len(tg) < 5:
                    continue
                yield rid, name, i, a, tg, cm


def main(corpus):
    resid, healthy_f3, bypass_f3, f45 = [], [], [], []
    for rid, name, i, a, tg, cm in rows(corpus):
        modern = rid.isdigit() and int(rid) >= ANCHOR_FIX_EPOCH
        f3 = a[2] - tg[2]
        if modern:
            f45.append((max(abs(a[3] - tg[3]), abs(a[4] - tg[4])), rid, name, i))
            (bypass_f3 if rid in BYPASS else healthy_f3).append((f3, rid, name, i))
        if len(cm) >= 2:
            L3 = max(500.0, a[0] + cm[0]) + cm[1]
            resid.append((f3 - max(0.0, L3 - 1000.0), rid, name, i))

    r = [x[0] for x in resid]
    print(f"C3 cliff fit  n={len(r)} ticks with capture_ms recorded")
    print(f"   residual f3 - max(0, L3-1000):  min {min(r):.0f}  median "
          f"{statistics.median(r):.0f}  p95 {sorted(r)[int(0.95 * len(r))]:.0f}  max {max(r):.0f} ms")
    print(f"   (residual is the CDP round-trip epsilon; it is NOT fitted)")

    hmax = max(healthy_f3)
    bsort = sorted(bypass_f3, reverse=True)
    print(f"\nC4 separation  healthy n={len(healthy_f3)}  bypass n={len(bypass_f3)}")
    print(f"   healthy MAX f3 = {hmax[0]:.0f} ms  (run.id {hmax[1]} {hmax[2]} tick {hmax[3]})")
    over = [x for x in bsort if x[0] > 150]
    print(f"   bypass f3 readings over the 150 ms bound: n={len(over)}")
    if over:
        lo = min(over)
        print(f"   bypass SMALLEST over-bound f3 = {lo[0]:.0f} ms (run.id {lo[1]} {lo[2]} tick {lo[3]})")
        print(f"   -> populations {'OVERLAP' if hmax[0] >= lo[0] else 'separate'}: "
              f"healthy max {hmax[0]:.0f} vs bypass min-over-bound {lo[0]:.0f}")

    v = sorted(x[0] for x in f45)
    print(f"\nC6 f4/f5 worst-of-pair  n={len(v)} ticks (post-#193-anchor runs only)")
    print(f"   min {v[0]:.0f}  median {statistics.median(v):.0f}  p95 "
          f"{v[int(0.95 * len(v))]:.0f}  max {max(f45)[0]:.0f} ms "
          f"(max at run.id {max(f45)[1]} {max(f45)[2]} tick {max(f45)[3]})")


if __name__ == "__main__":
    main(sys.argv[1])
