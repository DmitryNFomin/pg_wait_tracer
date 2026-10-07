"""Every figure in the report, derived from the committed walk JSONs.

`vol` below is the ONE thing not read out of the walk files: the whole-trace
event count and duration come from each run's `conditions-r*.txt`
(`pgwt-server — Summary    Events: N ... Duration: Ds`), r1's from
`conditions-r1-dump-2014utc.txt`. They are transcribed here rather than
parsed so the transcription is visible and checkable against those files.

The 900 s in-window figure is DERIVED (rate x 900 s), not a count:
`--dump` has no window argument, and a derived number is labelled as one.

Run: python3 summarise.py
"""
import json, statistics
runs = ("r1", "r2", "r3", "r4", "r5")
W = {r: json.load(open("walk-%s.json" % r)) for r in runs}
vol = {"r1": (4066533, 1055.1), "r2": (3993600, 1030.4), "r3": (3997696, 1031.9),
       "r4": (4047420, 1028.3), "r5": (4063232, 1028.9)}
print("per-run worst / rate / panel sizes")
for r in runs:
    res = W[r]["results"]
    worst = max(res, key=lambda t: max(res[t]["samples"]))
    ev, du = vol[r]
    print("  %s worst %4.0f %-11s events %d / %.1fs = %4.0f/s  in-window(900s,derived) %.2fM  sessions %-9s queries %-9s histogram %s"
          % (r, max(res[worst]["samples"]), worst, ev, du, ev / du, ev / du * 900 / 1e6,
             res["sessions"]["detail"], res["queries"]["detail"], res["histogram"]["detail"]))
w4 = [max(max(v["samples"]) for v in W[r]["results"].values()) for r in runs[:4]]
w5 = [max(max(v["samples"]) for v in W[r]["results"].values()) for r in runs]
print("\nn=4 per-run worst: %s median %.1f range %.0f -> margin %.2fx"
      % ([round(x) for x in w4], statistics.median(w4), max(w4) - min(w4), 3000 / max(w4)))
print("n=5 per-run worst: %s median %.1f range %.0f -> margin %.2fx"
      % ([round(x) for x in w5], statistics.median(w5), max(w5) - min(w5), 3000 / max(w5)))
print("\nper tab over all five walks (15 navigations each):")
agg = {}
for t in W["r1"]["results"]:
    s = [x for r in runs for x in W[r]["results"][t]["samples"]]
    agg[t] = (min(s), statistics.median(s), max(s), len(s))
for t, (a, m, b, n) in sorted(agg.items(), key=lambda kv: -kv[1][2]):
    print("  %-12s min %6.0f med %6.0f max %6.0f n=%d" % (t, a, m, b, n))
