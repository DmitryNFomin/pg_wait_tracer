#!/usr/bin/env python3
"""Issue #266 evidence: per-run geometry of the blink self-test's stimulus
relative to the sweep frames it is supposed to be seen by.

BEFORE (the old wall-clock window): the overlay was up for
[mount+700, mount+1100] and nothing recorded whether a frame was actually
taken inside it, so the margins below are computed from the frames' own
achieved offsets + capture_ms.

AFTER: the stimulus records its own bounds, so the margins are measured
directly, in the page's clock, and the verdict states whether the geometry
was proven.
"""
import glob
import json
import os
import statistics
import sys


def runs(d):
    paths = glob.glob(os.path.join(d, "ui_live_self_test_*", "summary.json"))
    return sorted(paths, key=lambda p: int(
        p.split("ui_live_self_test_")[1].split(os.sep)[0]))


def before(d):
    print("BEFORE -- old fixed window [mount+700, mount+1100]; the overlay's "
          "on-screen window was never measured.")
    print(f"{'run':>4} {'rc/caught':>9} {'ratio':>7} "
          f"{'f2_end-700':>11} {'f3_start-1100':>14} {'f3_end-1100':>12} "
          f"{'cap f2/f3':>12}")
    m_open, m_close_start, m_close_end = [], [], []
    for p in runs(d):
        s = json.load(open(p))
        st = s.get("self_test") or {}
        t = s["tabs"]["sessions"]["blink_sweep"]["ticks"][st.get("tick", 2) - 1]
        a, c = t["achieved_offsets_ms"], t["capture_ms"]
        f2e, f3s, f3e = a[1] + c[1], a[2], a[2] + c[2]
        m_open.append(700 - f2e)
        m_close_start.append(1100 - f3s)
        m_close_end.append(1100 - f3e)
        n = os.path.basename(os.path.dirname(p)).split("_")[-1]
        print(f"{n:>4} {str(st.get('caught')):>9} {st.get('ratio', 0):7.4f} "
              f"{700 - f2e:+11.0f} {1100 - f3s:+14.0f} {1100 - f3e:+12.0f} "
              f"{c[1]:6.0f}/{c[2]:<6.0f}")
    print(f"  margin before the window OPENS (700 - frame2 capture end): "
          f"min {min(m_open):+.0f} median {statistics.median(m_open):+.0f} "
          f"max {max(m_open):+.0f} ms   <=0 would mean the clean reference "
          f"frame may also have contained the overlay")
    print(f"  margin before the window CLOSES (1100 - frame3 capture start): "
          f"min {min(m_close_start):+.0f} median "
          f"{statistics.median(m_close_start):+.0f} max {max(m_close_start):+.0f} ms")
    print(f"  margin before the window CLOSES (1100 - frame3 capture END):   "
          f"min {min(m_close_end):+.0f} median "
          f"{statistics.median(m_close_end):+.0f} max {max(m_close_end):+.0f} ms   "
          f"negative => the target frame's raster instant MAY have fallen "
          f"outside the window; nothing measured which")


def after(d):
    print("AFTER -- the stimulus is bracketed around one frame's capture "
          "call and reports its own bounds (page clock).")
    print(f"{'run':>4} {'proven':>7} {'caught':>7} {'pairs':>14} "
          f"{'prev_end->show':>15} {'painted->start':>15} "
          f"{'end->hide':>10} {'cleared->next':>14} {'dwell':>7}")
    cols = {k: [] for k in ("a", "b", "c", "d", "dwell")}
    for p in runs(d):
        s = json.load(open(p))
        st = s.get("self_test") or {}
        if not st or st.get("stimulus") is None:
            print(f"{os.path.basename(os.path.dirname(p)):>4} "
                  f"NO SELF-TEST RECORD ({(s.get('tabs', {}).get('sessions') or {}).get('rendered')})")
            continue
        g, w = st["stimulus"], st["frame_windows_ms"]
        i = st["inject_frame_index"]
        a = g["shown_requested_ms"] - w[i - 1][1]
        b = w[i][0] - g["shown_painted_ms"]
        c = g["hidden_requested_ms"] - w[i][1]
        dd = (w[i + 1][0] - g["hidden_cleared_ms"]) if i + 1 < len(w) else None
        dwell = g["hidden_requested_ms"] - g["shown_painted_ms"]
        for k, v in zip(("a", "b", "c", "d", "dwell"), (a, b, c, dd, dwell)):
            if v is not None:
                cols[k].append(v)
        n = os.path.basename(os.path.dirname(p)).split("_")[-1]
        print(f"{n:>4} {str(st['stimulus_ok']):>7} {str(st['caught']):>7} "
              f"{str(st['boundary_pair_ratios']):>14} "
              f"{a:+15.0f} {b:+15.0f} {c:+10.0f} "
              f"{(f'{dd:+.0f}' if dd is not None else '-'):>14} {dwell:7.0f}")
    names = {"a": "prev frame capture end -> overlay append requested",
             "b": "overlay presented -> target capture call start",
             "c": "target capture end -> overlay removal requested",
             "d": "overlay gone -> next frame capture call start",
             "dwell": "measured on-screen dwell (presented -> removal requested)"}
    for k, label in names.items():
        v = cols[k]
        if v:
            print(f"  {label}: min {min(v):+.0f} median "
                  f"{statistics.median(v):+.0f} max {max(v):+.0f} ms (n={len(v)})")


if __name__ == "__main__":
    which, d = sys.argv[1], sys.argv[2]
    (before if which == "before" else after)(d)
