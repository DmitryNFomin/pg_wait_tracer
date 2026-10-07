#!/usr/bin/env python3
"""Pure verdict function over paint-walk JSON, plus its own bypass suite.

Why this is a separate pure function rather than a print inside the walk:
the walk's predecessor (scratchpad paint_walk.py, used for the 2079 ms pin)
computed its worst case as

    for t in TABS:
        r = results[t]
        if "error" in r: print("FAILED: ..."); continue    # <-- excluded
        worst = max(worst, r["max"])

so a tab that never rendered at all -- a dead bridge, a dead daemon, a
15 s PANEL_CHECK timeout -- was skipped, and the margin came out LARGER.
That is a false negative in exactly the direction that matters: the
component most likely to break is the one whose breakage made the number
look safe. A gate that cannot see must refuse, never approve.

`verdict()` therefore REFUSES (ok=False, with a reason) on every way the
evidence can be unreadable, and only reports a margin when every expected
tab has every expected sample:

    empty input, missing tab, errored tab, short sample count, a tab whose
    readiness detail is absent/unparseable or reports zero rendered items,
    an unasserted/unstable/wrong viewport, a window that is not the 15 m
    demo range, a dead workload process, a non-numeric timing.

Run the suite:  python3 paint_analyse.py --selftest
Analyse walks:  python3 paint_analyse.py walk-*.json
"""
import glob
import json
import re
import sys

BOUND_MS = 3000
EXPECTED_TABS = ("overview", "events", "sessions", "queries", "histogram",
                 "transitions", "concurrency", "waterfall", "scatter",
                 "matrix", "timeline")
EXPECTED_NAVS = 3
PINNED_VIEWPORT = (1710, 981, 2)
EXPECTED_RANGE = "15m"


def rendered_items(detail):
    """Leading integer of a PANEL_CHECKS detail string ('412 rows',
    '183 cells', '5 nodes, 7 edges'), or None when there is no number to
    read. None is NOT zero and must not be treated as a pass: a detail
    the analyser cannot parse is a detail it cannot vouch for."""
    if not isinstance(detail, str):
        return None
    m = re.match(r"\s*(\d+)", detail)
    return int(m.group(1)) if m else None


def verdict(walks, bound_ms=BOUND_MS, expected_tabs=EXPECTED_TABS,
            expected_navs=EXPECTED_NAVS):
    """Return {ok, reason, worst_ms, worst_tab, margin, per_tab, per_walk}.

    ok=False means REFUSE: the evidence does not support any margin claim.
    It never means "fine, minus the bits we could not read"."""
    def refuse(reason):
        return {"ok": False, "reason": reason, "worst_ms": None,
                "worst_tab": None, "margin": None, "per_tab": {},
                "per_walk": []}

    if not walks:
        return refuse("no walks: zero input files produced zero evidence")

    per_walk = []
    samples_by_tab = {t: [] for t in expected_tabs}
    for i, w in enumerate(walks):
        label = w.get("label", "walk%d" % i)
        if not w.get("viewport_agree"):
            return refuse("%s: viewport not confirmed by two agreeing reads" % label)
        vp = w.get("viewport") or {}
        if (vp.get("w"), vp.get("h"), vp.get("dpr")) != PINNED_VIEWPORT:
            return refuse("%s: viewport %r is not the pinned %r" % (label, vp, PINNED_VIEWPORT))
        if w.get("range") != EXPECTED_RANGE:
            return refuse("%s: active range %r is not the %s demo window"
                          % (label, w.get("range"), EXPECTED_RANGE))
        if w.get("workload_alive") is not True:
            return refuse("%s: workload/daemon liveness not confirmed -- paint "
                          "measured against a possibly idle system" % label)
        res = w.get("results") or {}
        for tab in expected_tabs:
            if tab not in res:
                return refuse("%s: tab %s absent from the results -- absent is "
                              "not fast" % (label, tab))
            r = res[tab]
            if r.get("error"):
                return refuse("%s: tab %s did not render (%s) -- a tab that "
                              "never painted cannot be excluded from the worst "
                              "case" % (label, tab, r["error"]))
            vals = r.get("samples")
            if not isinstance(vals, list) or len(vals) != expected_navs:
                return refuse("%s: tab %s has %r samples, expected %d"
                              % (label, tab, (len(vals) if isinstance(vals, list) else vals),
                                 expected_navs))
            if not all(isinstance(v, (int, float)) and v > 0 for v in vals):
                return refuse("%s: tab %s has a non-numeric or non-positive "
                              "timing: %r" % (label, tab, vals))
            n = rendered_items(r.get("detail"))
            if n is None:
                return refuse("%s: tab %s readiness detail %r carries no item "
                              "count -- unparseable evidence is not evidence"
                              % (label, tab, r.get("detail")))
            if n == 0:
                return refuse("%s: tab %s painted 0 items -- an empty panel's "
                              "paint is not the demo's paint" % (label, tab))
            samples_by_tab[tab].extend(vals)
        per_walk.append({"label": label,
                         "worst_ms": max(max(res[t]["samples"]) for t in expected_tabs),
                         "worst_tab": max(expected_tabs,
                                          key=lambda t: max(res[t]["samples"]))})

    per_tab = {}
    for tab in expected_tabs:
        v = sorted(samples_by_tab[tab])
        mid = len(v) // 2
        med = v[mid] if len(v) % 2 else (v[mid - 1] + v[mid]) / 2.0
        per_tab[tab] = {"min": v[0], "med": med, "max": v[-1], "n": len(v)}
    worst_tab = max(expected_tabs, key=lambda t: per_tab[t]["max"])
    worst = per_tab[worst_tab]["max"]
    return {"ok": True, "reason": "complete: %d walks x %d tabs x %d navs"
            % (len(walks), len(expected_tabs), expected_navs),
            "worst_ms": worst, "worst_tab": worst_tab,
            "margin": bound_ms / worst, "per_tab": per_tab,
            "per_walk": per_walk}


# --------------------------------------------------------------------------
# Bypass suite: one case per way this verdict's detection can be made
# unreachable. Each case is a walk set that a naive analyser would happily
# turn into a reassuring margin.
# --------------------------------------------------------------------------
def _ok_walk(label="w", worst=1500.0):
    res = {}
    for t in EXPECTED_TABS:
        res[t] = {"samples": [400.0, 450.0, 500.0], "detail": "412 rows"}
    res["waterfall"]["samples"] = [worst - 200, worst - 100, worst]
    return {"label": label, "viewport": {"w": 1710, "h": 981, "dpr": 2},
            "viewport_agree": True, "range": "15m", "workload_alive": True,
            "results": res}


def _selftest():
    import copy
    cases = []

    cases.append(("empty input", [], "no walks"))

    w = _ok_walk()
    del w["results"]["scatter"]
    cases.append(("tab absent entirely (not wrong -- missing)", [w], "absent from the results"))

    w = _ok_walk()
    w["results"]["waterfall"] = {"error": "timeout"}
    cases.append(("tab never rendered (dead bridge/daemon)", [w], "did not render"))

    w = _ok_walk()
    w["results"]["queries"]["samples"] = [410.0]
    cases.append(("partial nav range (1 of 3 navigations)", [w], "expected 3"))

    w = _ok_walk()
    w["results"]["queries"]["detail"] = "rows"
    cases.append(("unparseable readiness detail", [w], "carries no item count"))

    w = _ok_walk()
    w["results"]["queries"]["detail"] = None
    cases.append(("absent readiness detail", [w], "carries no item count"))

    w = _ok_walk()
    w["results"]["events"]["detail"] = "0 rows"
    cases.append(("panel painted nothing (empty, not slow)", [w], "painted 0 items"))

    w = _ok_walk()
    w["viewport"] = {"w": 1200, "h": 937, "dpr": 2}
    cases.append(("stale-zoom viewport (smaller = less work)", [w], "is not the pinned"))

    w = _ok_walk()
    w["viewport_agree"] = False
    cases.append(("viewport read once, mid-animation", [w], "two agreeing reads"))

    w = _ok_walk()
    w["range"] = "2m"
    cases.append(("wrong window (cheap 2 m instead of 15 m)", [w], "demo window"))

    w = _ok_walk()
    w["workload_alive"] = False
    cases.append(("workload died -- paint against an idle system", [w], "liveness not confirmed"))

    w = _ok_walk()
    w["results"]["matrix"]["samples"] = [400.0, None, 500.0]
    cases.append(("non-numeric timing", [w], "non-numeric"))

    w = _ok_walk()
    w["results"]["matrix"]["samples"] = [400.0, 0, 500.0]
    cases.append(("zero-ms timing (nothing was actually measured)", [w], "non-numeric or non-positive"))

    failures = 0
    for name, walks, needle in cases:
        v = verdict(copy.deepcopy(walks))
        if v["ok"]:
            print("FAIL  %-50s -> APPROVED (should refuse)" % name)
            failures += 1
        elif needle not in v["reason"]:
            print("FAIL  %-50s -> refused for the wrong reason: %s" % (name, v["reason"]))
            failures += 1
        else:
            print("ok    %-50s -> refused: %s" % (name, v["reason"][:70]))

    # True-positive path: a complete walk set DOES produce a verdict, and a
    # slow tab IS reported (a refuser that refuses everything is useless).
    v = verdict([_ok_walk("a", 1500.0), _ok_walk("b", 1600.0)])
    if not (v["ok"] and v["worst_tab"] == "waterfall" and abs(v["worst_ms"] - 1600.0) < 1e-9):
        print("FAIL  complete input -> %r" % v); failures += 1
    else:
        print("ok    %-50s -> worst %s %.0f ms margin %.2fx"
              % ("complete input is accepted", v["worst_tab"], v["worst_ms"], v["margin"]))
    v = verdict([_ok_walk("a", 4000.0)])
    if not (v["ok"] and v["worst_ms"] == 4000.0 and v["margin"] < 1.0):
        print("FAIL  over-bound input not reported: %r" % v); failures += 1
    else:
        print("ok    %-50s -> margin %.2fx (< 1.0, i.e. OVER)"
              % ("an over-bound walk is reported, not hidden", v["margin"]))

    print("\n%d case(s) FAILED" % failures if failures else "\nall cases passed")
    return 1 if failures else 0


def main(argv):
    if "--selftest" in argv:
        return _selftest()
    files = []
    for pat in argv[1:]:
        files.extend(sorted(glob.glob(pat)))
    walks = []
    for f in files:
        with open(f) as fh:
            w = json.load(fh)
        w.setdefault("label", f)
        walks.append(w)
    v = verdict(walks)
    print("files: %s" % ", ".join(files))
    if not v["ok"]:
        print("VERDICT: REFUSED -- %s" % v["reason"])
        print(json.dumps(v, indent=2, default=str))
        return 1
    print("VERDICT: %s" % v["reason"])
    print("\n%-12s %7s %7s %7s %4s" % ("tab", "min", "med", "max", "n"))
    for t in sorted(EXPECTED_TABS, key=lambda t: -v["per_tab"][t]["max"]):
        r = v["per_tab"][t]
        print("%-12s %7.0f %7.0f %7.0f %4d" % (t, r["min"], r["med"], r["max"], r["n"]))
    print("\nper-walk worst: %s" % ", ".join(
        "%s %.0f (%s)" % (w["label"], w["worst_ms"], w["worst_tab"]) for w in v["per_walk"]))
    print("worst overall: %.0f ms (%s); bound %d ms; margin %.2fx"
          % (v["worst_ms"], v["worst_tab"], BOUND_MS, v["margin"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
