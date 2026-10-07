"""Per-run MINIMUM tick-1 headroom to the 1000 ms clamp, over the five heavy
tabs -- the quantity that decides whether a whole box-check run goes red on
frame_spacing, since any one frame over the bound fails the gate.

headroom = 1000 - (max(500, A + s1) + s2), all recorded in summary.json.
Positive -> frame 3 waits for its target and reads ~9 ms.  Negative -> frame 3
fires late by |headroom| and reads |headroom| + ~9 ms.

box class is fingerprinted by the run's own median per-frame capture cost
(bimodal across the corpus: ~45-100 ms vs ~120-300 ms), not asserted from a
hostname the artifact does not record.
"""
import datetime
import json
import os
import statistics
import sys

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]
BYPASS = {"1790798813", "1790798974"}


def main(corpus):
    print(f"{'when':<13}{'run.id':<12}{'med s':>6}{'class':>7}"
          f"{'min headroom t1':>16}{'at':>14}{'run red?':>10}")
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p) or not rid.isdigit() or int(rid) < 1790700000:
            continue
        with open(p) as f:
            d = json.load(f)
        alls, hs = [], []
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
            for t in ticks:
                alls.extend(t.get("capture_ms") or [])
            if not ticks:
                continue
            t = ticks[0]
            a, cm = t.get("achieved_offsets_ms") or [], t.get("capture_ms") or []
            if len(a) < 3 or len(cm) < 2:
                continue
            hs.append((1000.0 - (max(500.0, a[0] + cm[0]) + cm[1]), name))
        if not hs or not alls:
            continue
        meds = statistics.median(alls)
        cls = "slow" if meds >= 110 else "fast"
        lo = min(hs)
        red = (d.get("ok") is False)
        w = datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M")
        tag = "  BYPASS" if rid in BYPASS else ""
        print(f"{w:<13}{rid:<12}{meds:>6.0f}{cls:>7}{lo[0]:>16.0f}{lo[1]:>14}"
              f"{str(red):>10}{tag}")

    print("\nmin-headroom summary, HEALTHY 'slow' (gate-1-class) runs only:")
    vals = []
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p) or not rid.isdigit() or int(rid) < 1790700000 or rid in BYPASS:
            continue
        with open(p) as f:
            d = json.load(f)
        alls, hs = [], []
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
            for t in ticks:
                alls.extend(t.get("capture_ms") or [])
            if not ticks:
                continue
            a, cm = ticks[0].get("achieved_offsets_ms") or [], ticks[0].get("capture_ms") or []
            if len(a) >= 3 and len(cm) >= 2:
                hs.append(1000.0 - (max(500.0, a[0] + cm[0]) + cm[1]))
        if hs and alls and statistics.median(alls) >= 110:
            vals.append(min(hs))
    vals.sort()
    print(f"   n={len(vals)} runs: {[round(v) for v in vals]}")
    print(f"   median {statistics.median(vals):.0f} ms, "
          f"{sum(1 for v in vals if v < 0)}/{len(vals)} already negative")


if __name__ == "__main__":
    main(sys.argv[1])
