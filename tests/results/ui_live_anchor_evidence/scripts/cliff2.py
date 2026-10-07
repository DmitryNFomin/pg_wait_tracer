"""The f3 cliff, from the recorded numbers only.

The sweep is SERIAL: _capture_at_offset sleeps only the REMAINDER to
mount+offset, then screenshots.  So, with A = achieved[0] (mount -> just
before screenshot 1) and s_k = capture_ms[k-1] (screenshot k's own cost):

    achieved[1] = max(500,  A + s1 + e)
    achieved[2] = max(1000, achieved[1] + s2 + e)        e ~ 5-8 ms CDP hop

f3 drift = achieved[2] - 1000, which is > 0 exactly when
    max(500, A + s1) + s2 > 1000.
So f3 is a CLIFF on one quantity, L := max(500, A + s1) + s2, not a trend.
This prints L and its distance to the 1000 ms edge, per heavy tab per run,
so the cliff is checkable rather than asserted.

Frames 4 and 5 (targets 1500/2000, i.e. 500 ms gaps) are the frames that are
NOT on a cliff at these capture costs -- which is why they are the honest
read on whether paint regressed.
"""
import datetime
import json
import os
import sys

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]


def runs(corpus):
    out = []
    for rid in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p):
            continue
        with open(p) as f:
            out.append((rid, json.load(f), open(os.path.join(corpus, rid, "origin.txt")).read().strip()))
    return out


def when(rid):
    return (datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M")
            if rid.isdigit() else rid[:11])


def main(corpus, mode):
    print(f"{'when':<13}{'run.id':<12}{'tab':<13}{'ttfp':>6}{'A':>6}{'s1':>6}{'s2':>6}"
          f"{'L':>7}{'edge':>7}{'f3':>6}{'f4':>5}{'f5':>5}")
    for rid, d, _origin in runs(corpus):
        for name in HEAVY:
            tab = (d.get("tabs") or {}).get(name)
            if not tab:
                continue
            ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
            if not ticks:
                continue
            t = ticks[0]
            a = t.get("achieved_offsets_ms") or []
            tg = t.get("target_offsets_ms") or []
            cm = t.get("capture_ms") or []
            if len(a) < 5 or len(tg) < 5:
                continue
            A = a[0]
            s1 = cm[0] if len(cm) > 0 else None
            s2 = cm[1] if len(cm) > 1 else None
            if s1 is None or s2 is None:
                L = edge = None
            else:
                L = max(500.0, A + s1) + s2
                edge = 1000.0 - L
            ttfp = tab.get("ttfp_ms")
            f = lambda v, w=6: (f"{v:>{w}.0f}" if isinstance(v, (int, float)) else f"{'-':>{w}}")
            if mode == "all" or (mode == "heavy" and name in HEAVY):
                print(f"{when(rid):<13}{rid:<12}{name:<13}{f(ttfp)}{f(A)}{f(s1)}{f(s2)}"
                      f"{f(L, 7)}{f(edge, 7)}{f(a[2] - tg[2])}{f(a[3] - tg[3], 5)}{f(a[4] - tg[4], 5)}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "heavy")
