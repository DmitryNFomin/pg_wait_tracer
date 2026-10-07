"""Tick-1 excess: A(tick 1) - median A(ticks 2..6), per tab per run.

A = achieved_offsets_ms[0] = mount -> just-before-screenshot-1.  Ticks 2-6
share one steady value (the blind-window 100 ms sleep + render check + clip +
CDP hops).  Tick 1 adds whatever ran between its mount and the loop.  If the
driver's `page.wait_for_timeout(500)` after the live-resume click is the cause,
the excess clusters near 500 ms and ONLY on the tabs whose views pause live
(app.js pausesLive: transitions, concurrency, waterfall, scatter, matrix) plus
timeline (reached by drill(), which also calls stopAutoRefresh).
"""
import datetime
import json
import os
import statistics
import sys

PAUSES_LIVE = {"transitions", "concurrency", "waterfall", "scatter", "matrix", "timeline"}
ORDER = ["scatter", "matrix", "timeline", "transitions", "concurrency", "waterfall",
         "overview", "events", "sessions", "queries", "histogram"]


def a_series(tab):
    out = []
    for t in (tab.get("blink_sweep") or {}).get("ticks") or []:
        a = t.get("achieved_offsets_ms") or []
        out.append(a[0] if a else None)
    return out


def main(corpus, only):
    print(f"{'when':<13}{'run.id':<12}{'tab':<12}{'pausesLive':>11}"
          f"{'A t1':>7}{'A t2-6 med':>12}{'excess':>8}")
    for rid in sorted(os.listdir(corpus)):
        if only and rid not in only:
            continue
        p = os.path.join(corpus, rid, "summary.json")
        if not os.path.exists(p):
            continue
        with open(p) as f:
            d = json.load(f)
        w = datetime.datetime.fromtimestamp(int(rid)).strftime("%m-%d %H:%M") if rid.isdigit() else rid
        tabs = d.get("tabs") or {}
        for name in ORDER:
            tab = tabs.get(name)
            if not tab:
                continue
            s = [x for x in a_series(tab) if x is not None]
            if len(s) < 3:
                continue
            rest = s[1:]
            med = statistics.median(rest)
            print(f"{w:<13}{rid:<12}{name:<12}{str(name in PAUSES_LIVE):>11}"
                  f"{s[0]:>7.0f}{med:>12.0f}{s[0] - med:>8.0f}")


if __name__ == "__main__":
    main(sys.argv[1], set(sys.argv[2:]))
