#!/usr/bin/env python3
"""The false negative, on REAL data: one real dead tab inside a real walk.

`redlive-bridge-down.json` alone is refused at the first gate (the app never
loaded, so the active range reads '?'), which proves the refusal but not the
specific hole that mattered. This splices the REAL errored `scatter` entry
from that file into the REAL, otherwise-complete `walk-r1.json` -- the
situation where the bridge drops for one tab mid-walk and recovers. Then:

  * the predecessor's arithmetic drops scatter and reports the max over the
    tabs that did work -- a SMALLER worst case and a LARGER margin than the
    truth, on a walk where the slowest tab never painted at all;
  * paint_analyse.verdict() refuses.

Run: python3 red_splice.py
"""
import copy
import json

import paint_analyse as pa
import red_demo_predecessor as rd

real = json.load(open("walk-r1.json"))
dead = json.load(open("redlive-bridge-down.json"))

print("real walk r1, intact:")
v = pa.verdict([copy.deepcopy(real)])
o = rd.predecessor_verdict([copy.deepcopy(real)])
print("  predecessor   : worst %.0f ms, margin %.2fx" % (o["worst_ms"], o["margin"]))
print("  paint_analyse : ok=%s worst %s (%s) margin %.2fx"
      % (v["ok"], round(v["worst_ms"]), v["worst_tab"], v["margin"]))

spliced = copy.deepcopy(real)
# The real failed-tab record produced by a real dead bridge, verbatim.
spliced["results"]["scatter"] = copy.deepcopy(dead["results"]["waterfall"])
spliced["label"] = "r1-with-a-real-dead-scatter"
print("\nsame walk, scatter replaced by the REAL dead-bridge record")
print("  spliced scatter entry: %r" % spliced["results"]["scatter"])
o = rd.predecessor_verdict([copy.deepcopy(spliced)])
v = pa.verdict([copy.deepcopy(spliced)])
print("  predecessor   : worst %.0f ms, margin %.2fx  <-- scatter (the real worst "
      "tab, 1616 ms) never painted, and the margin went UP"
      % (o["worst_ms"], o["margin"]))
print("  paint_analyse : ok=%s  %s" % (v["ok"], v["reason"]))
