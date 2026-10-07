"""Does FASTER first paint make tick-1 drift WORSE?

Mechanism under test: _navigate_to_tab's live-resume branch clicks #live-btn and
then sleeps a FIXED 500 ms.  That click triggers a refresh, hence an AAS send
(recorded as tick 1 by TICK_HOOK_JS) and a mount.  The sweep then anchors on
that mount.  So tick-1 A = (500 ms sleep + leak probe + 100 ms blind window +
render check + clip) MINUS however long the view took to mount after the click.

Prediction: tick-1 A is ANTI-correlated with ttfp_ms -- a tab that paints
faster leaves more of the fixed 500 ms sleep on the far side of its own mount,
so its sweep anchor is MORE stale and its frame-3 drift LARGER.

Restricted to gate-1-class runs (median per-frame capture cost >= 110 ms) so
machine class is not the thing being correlated.
"""
import datetime
import json
import os
import statistics
import sys

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]
BYPASS = {"1790798813", "1790798974"}


def pearson(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    dx = sum((x - mx) ** 2 for x in xs) ** 0.5
    dy = sum((y - my) ** 2 for y in ys) ** 0.5
    return num / (dx * dy) if dx and dy else float("nan")


def main(corpus):
    pts = []   # (ttfp, A_tick1, rid, tab)
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p) or not rid.isdigit() or rid in BYPASS:
            continue
        with open(p) as f:
            d = json.load(f)
        allcm = []
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            for t in (tab.get("blink_sweep") or {}).get("ticks") or []:
                allcm.extend(t.get("capture_ms") or [])
        if not allcm or statistics.median(allcm) < 110:
            continue
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
            ttfp = tab.get("ttfp_ms")
            if not ticks or not isinstance(ttfp, (int, float)):
                continue
            a = ticks[0].get("achieved_offsets_ms") or []
            if a:
                pts.append((float(ttfp), float(a[0]), rid, name))

    print("transitions, gate-1-class runs, chronological "
          "(ae7e9d5 = #291/#292 transitions paint fix merged 2026-10-06 11:01 UTC):")
    for ttfp, A, rid, name in sorted(p for p in pts if p[3] == "transitions"):
        w = datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M")
        print(f"   {w}  run.id={rid}  ttfp={ttfp:6.0f} ms   tick-1 A={A:5.0f} ms")

    print(f"\nPearson r(ttfp_ms, tick-1 A), all heavy tabs, gate-1-class, n={len(pts)}: "
          f"{pearson([p[0] for p in pts], [p[1] for p in pts]):+.2f}")
    for name in HEAVY:
        sub = [p for p in pts if p[3] == name]
        if len(sub) >= 4:
            print(f"   {name:<13} n={len(sub)}  r={pearson([p[0] for p in sub], [p[1] for p in sub]):+.2f}")


if __name__ == "__main__":
    main(sys.argv[1])
