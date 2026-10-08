#!/usr/bin/env python3
"""Tabulate one paint walk against the 2026-10-07 pinned per-tab maxima.

The pinned column is read from ../demo_paint_20261007/SUMMARY.txt's "per tab
over all five walks" block (15 navigations per tab), transcribed here so this
file is self-contained; the source is cited in README.md.

Usage: compare.py walk-rN.json
"""
import json
import sys

PINNED_MAX = {
    "scatter": 1616, "queries": 1592, "waterfall": 1464, "concurrency": 1165,
    "matrix": 1139, "transitions": 971, "timeline": 441, "histogram": 440,
    "overview": 395, "events": 266, "sessions": 264,
}
BOUND_MS = 3000.0


def main():
    d = json.load(open(sys.argv[1]))
    print("label=%s viewport=%s agree=%s range=%s chrome=%s"
          % (d.get("label"), d.get("viewport"), d.get("viewport_agree"),
             d.get("range"), d.get("chrome")))
    print("workload_alive=%s basis=%r" % (d.get("workload_alive"),
                                          d.get("workload_alive_basis")))
    print("capture_age_s=%s loadavg=%s trace_events_total=%s"
          % (d.get("capture_age_s"), d.get("loadavg"),
             d.get("trace_events_total")))
    print()
    rows = []
    for tab, r in d["results"].items():
        s = [x for x in (r.get("samples") or []) if isinstance(x, (int, float))]
        rows.append((max(s) if s else -1.0, tab, r, s))
    hdr = ("%-12s %2s %7s %7s %7s | %7s %6s | %-5s | %s"
           % ("tab", "n", "min", "med", "max", "pinned", "ratio", "state",
              "detail / error"))
    print(hdr)
    print("-" * len(hdr))
    for mx, tab, r, s in sorted(rows, reverse=True):
        pm = PINNED_MAX.get(tab)
        ratio = (mx / pm) if (pm and mx > 0) else 0.0
        med = sorted(s)[len(s) // 2] if s else 0.0
        state = "OVER" if mx > BOUND_MS else "ok"
        if r.get("error"):
            state = "ERR"
        print("%-12s %2d %7.0f %7.0f %7.0f | %7s %6.2f | %-5s | %s %s"
              % (tab, len(s), min(s) if s else 0, med, mx,
                 pm if pm else "-", ratio, state,
                 r.get("detail") or "", r.get("error") or ""))
    print()
    allmax = max(r[0] for r in rows)
    over = sorted(t for m, t, _, _ in rows if m > BOUND_MS)
    errs = sorted(t for _, t, r, _ in rows if r.get("error"))
    print("worst single navigation : %.0f ms" % allmax)
    print("bound                   : %.0f ms" % BOUND_MS)
    print("margin (bound / worst)  : %.2fx   %s"
          % (BOUND_MS / allmax, "OVER THE BOUND" if allmax > BOUND_MS else "under"))
    print("tabs over the bound (%d) : %s" % (len(over), ", ".join(over)))
    print("tabs that failed to render on >=1 navigation (%d): %s"
          % (len(errs), ", ".join(errs) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
