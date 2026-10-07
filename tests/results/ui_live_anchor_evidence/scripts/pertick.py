"""A = achieved[0] (mount -> just before screenshot 1) for EVERY tick, not just
tick 1 -- to separate one-off page-startup cost from per-tick harness cost, and
to show which gated frames sit on the 1000 ms / 500 ms clamp cliffs.

Also prints, per tick, L3 = max(500, A+s1)+s2 (the quantity whose crossing of
1000 ms makes frame 3's drift jump from ~e to 100-300 ms) and L2 = A+s1 (whose
crossing of 500 ms does the same to frame 2, which IS gated on ticks 2-6).
"""
import datetime
import json
import os
import sys

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]


def main(corpus, only=None):
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p):
            continue
        if only and rid not in only:
            continue
        with open(p) as f:
            d = json.load(f)
        w = datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M") if rid.isdigit() else rid
        print(f"\n=== {w}  run.id={rid}")
        print(f"{'tab':<13}{'tick':>5}{'A':>6}{'s1':>6}{'s2':>6}"
              f"{'L2=A+s1':>9}{'L3':>7}{'f2':>6}{'f3':>6}{'f4':>5}{'f5':>5}")
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            for i, t in enumerate((tab.get("blink_sweep") or {}).get("ticks") or [], 1):
                a = t.get("achieved_offsets_ms") or []
                tg = t.get("target_offsets_ms") or []
                cm = t.get("capture_ms") or []
                if len(a) < 5 or len(tg) < 5 or len(cm) < 2:
                    continue
                A, s1, s2 = a[0], cm[0], cm[1]
                L2 = A + s1
                L3 = max(500.0, L2) + s2
                print(f"{name:<13}{i:>5}{A:>6.0f}{s1:>6.0f}{s2:>6.0f}"
                      f"{L2:>9.0f}{L3:>7.0f}{a[1] - tg[1]:>6.0f}{a[2] - tg[2]:>6.0f}"
                      f"{a[3] - tg[3]:>5.0f}{a[4] - tg[4]:>5.0f}")


if __name__ == "__main__":
    main(sys.argv[1], set(sys.argv[2:]) or None)
