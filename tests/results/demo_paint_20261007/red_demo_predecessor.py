#!/usr/bin/env python3
"""Shows what RED looks like: the SAME bypass cases, against the predecessor.

`predecessor_verdict` below is a verbatim transcription of the worst-case
computation in the harness that produced the pinned 2079 ms figure
(the orchestrator's scratchpad `paint_walk.py`):

    worst = 0
    for t in TABS:
        r = results[t]
        if "error" in r: print("%-12s  %s" % (t, "FAILED: "+r["error"][:50])); continue
        worst = max(worst, r["max"])
        ...
    print("worst max across all tabs: %.0f ms (bound %d ms, margin %.2fx)"
          % (worst, BOUND_MS, (BOUND_MS/worst if worst else 0)))

It prints a margin unconditionally. It skips errored tabs, never looks at
the readiness detail, never looks at the sample count, and never looks at
viewport / range / liveness at all (those were printed but not gating).
So every one of the cases below -- a dead bridge, a 2 m window instead of
15 m, a panel that painted zero rows, a dead workload -- comes out as a
reassuring margin.

Run:  python3 red_demo_predecessor.py
"""
import copy
import sys

import paint_analyse as pa

BOUND_MS = 3000
TABS = pa.EXPECTED_TABS


def predecessor_verdict(walks):
    """The predecessor's arithmetic, on the same input shape."""
    worst = 0
    for w in walks:
        res = w.get("results") or {}
        for t in TABS:
            r = res.get(t)
            if r is None:
                continue                       # absent tab: silently skipped
            if "error" in r:
                continue                       # errored tab: EXCLUDED from worst
            vals = [v for v in (r.get("samples") or []) if isinstance(v, (int, float))]
            if not vals:
                continue
            worst = max(worst, max(vals))
    return {"ok": True, "worst_ms": worst,
            "margin": (BOUND_MS / worst if worst else 0)}


def main():
    cases = _cases()
    print("%-50s %-28s %s" % ("bypass case", "predecessor", "paint_analyse.verdict"))
    print("-" * 110)
    leaked = 0
    for name, walks in cases:
        old = predecessor_verdict(copy.deepcopy(walks))
        new = pa.verdict(copy.deepcopy(walks))
        old_s = ("APPROVED worst %.0f margin %.2fx" % (old["worst_ms"], old["margin"])
                 if old["worst_ms"] else "APPROVED worst 0 margin 0.00x")
        new_s = "REFUSED" if not new["ok"] else "approved"
        if not new["ok"]:
            leaked += 1
        print("%-50s %-28s %s" % (name[:50], old_s, new_s))
    print("-" * 110)
    print("%d/%d cases the predecessor turned into a margin, and this analyser "
          "refuses." % (leaked, len(cases)))
    return 0 if leaked == len(cases) else 1


def _cases():
    """Same construction as paint_analyse._selftest, as data."""
    out = []
    out.append(("empty input", []))

    w = pa._ok_walk(); del w["results"]["scatter"]
    out.append(("tab absent entirely", [w]))
    w = pa._ok_walk(); w["results"]["waterfall"] = {"error": "timeout"}
    out.append(("tab never rendered (dead bridge/daemon)", [w]))
    w = pa._ok_walk(); w["results"]["queries"]["samples"] = [410.0]
    out.append(("partial nav range (1 of 3)", [w]))
    w = pa._ok_walk(); w["results"]["queries"]["detail"] = "rows"
    out.append(("unparseable readiness detail", [w]))
    w = pa._ok_walk(); w["results"]["queries"]["detail"] = None
    out.append(("absent readiness detail", [w]))
    w = pa._ok_walk(); w["results"]["events"]["detail"] = "0 rows"
    out.append(("panel painted nothing (empty, not slow)", [w]))
    w = pa._ok_walk(); w["viewport"] = {"w": 1200, "h": 937, "dpr": 2}
    out.append(("stale-zoom viewport", [w]))
    w = pa._ok_walk(); w["viewport_agree"] = False
    out.append(("viewport read once, mid-animation", [w]))
    w = pa._ok_walk(); w["range"] = "2m"
    out.append(("wrong window (2 m, not 15 m)", [w]))
    w = pa._ok_walk(); w["workload_alive"] = False
    out.append(("workload died (idle system)", [w]))
    w = pa._ok_walk(); w["results"]["matrix"]["samples"] = [400.0, None, 500.0]
    out.append(("non-numeric timing", [w]))
    w = pa._ok_walk(); w["results"]["matrix"]["samples"] = [400.0, 0, 500.0]
    out.append(("zero-ms timing", [w]))
    return out


if __name__ == "__main__":
    sys.exit(main())
