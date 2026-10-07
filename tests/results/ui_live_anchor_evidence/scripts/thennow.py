"""THEN-vs-NOW, per heavy tab: tick-1 drift profile, ttfp_ms, and the per-tick
harness cost A -- THEN = the 5 runs the FRAME_SPACING_DRIFT_BOUND_MS comment
names as its calibration set, NOW = the three 2026-10-07 runs.

Also counts the frames frame_spacing_ok actually gates, so a coverage claim is
counted rather than asserted: tick 1 frames 3-5 plus ticks 2-6 frames 2-5.
"""
import datetime
import json
import os
import statistics
import sys

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]
THEN = ["1790796035", "1790799596", "1790796123", "1790799745", "1790801156"]
NOW = ["1791354539", "1791385783", "1791390326"]


def load(corpus, rid):
    with open(os.path.join(corpus, rid, "summary.json")) as f:
        return json.load(f)


def gated_frame_count(d):
    """Exactly frame_spacing_ok's own scope: start_idx = 2 if tick 1 else 1,
    through index 4 (frame 5), truncated by what the tick actually recorded."""
    n = 0
    for tab in (d.get("tabs") or {}).values():
        for i, t in enumerate((tab.get("blink_sweep") or {}).get("ticks") or [], 1):
            tg = t.get("target_offsets_ms") or []
            a = t.get("achieved_offsets_ms") or []
            drift_len = min(len(tg), len(a))
            start = 2 if i == 1 else 1
            n += len(range(drift_len)[start:5])
    return n


def main(corpus):
    for label, rids in (("THEN (calibration set)", THEN), ("NOW (2026-10-07)", NOW)):
        print(f"\n### {label}")
        print(f"{'run.id':<12}{'when':<13}{'tab':<13}{'ttfp_ms':>8}"
              f"{'t1 drift f1..f5':<26}{'A t1':>6}{'A t2-6':>8}{'gated frames':>13}")
        for rid in rids:
            try:
                d = load(corpus, rid)
            except FileNotFoundError:
                print(f"{rid:<12}NOT RETAINED")
                continue
            w = datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M")
            gf = gated_frame_count(d)
            first = True
            for name in HEAVY:
                tab = (d.get("tabs") or {}).get(name)
                if not tab:
                    continue
                ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
                if not ticks:
                    continue
                t = ticks[0]
                a, tg = t.get("achieved_offsets_ms") or [], t.get("target_offsets_ms") or []
                nmin = min(len(a), len(tg))
                dr = [round(a[k] - tg[k]) for k in range(nmin)]
                As = [x.get("achieved_offsets_ms")[0] for x in ticks[1:]
                      if x.get("achieved_offsets_ms")]
                ttfp = tab.get("ttfp_ms")
                ts = f"{ttfp:.0f}" if isinstance(ttfp, (int, float)) else "ABSENT"
                print(f"{(rid if first else ''):<12}{(w if first else ''):<13}{name:<13}{ts:>8}"
                      f"{str(dr):<26}{a[0] if a else 0:>6.0f}"
                      f"{(statistics.median(As) if As else float('nan')):>8.0f}"
                      f"{(gf if first else ''):>13}")
                first = False


if __name__ == "__main__":
    main(sys.argv[1])
