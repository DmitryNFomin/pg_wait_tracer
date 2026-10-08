#!/usr/bin/env python3
"""Merge a run's box-side conditions into its walk JSON.

workload_alive is set to True only when the conditions log shows EVERY one of
the three processes (pgbench, the Python workload, the daemon) alive and no
DEAD line. An unreadable conditions file leaves it False -- an absent answer
is not a yes, and paint_analyse.py then refuses the walk.
"""
import json
import re
import sys

walk_path, cond_path = sys.argv[1], sys.argv[2]
with open(walk_path) as f:
    walk = json.load(f)
try:
    cond = open(cond_path).read()
except OSError as e:
    cond = ""
    print("could not read conditions: %s" % e)

alive = len(re.findall(r"^alive \d+:", cond, re.M))
dead = len(re.findall(r"^DEAD ", cond, re.M))
walk["workload_alive"] = (alive == 3 and dead == 0)
walk["conditions_alive_count"] = alive
walk["conditions_dead_count"] = dead

m = re.search(r"capture_age_s=(\d+)", cond)
walk["capture_age_s"] = int(m.group(1)) if m else None
m = re.search(r"loadavg=([\d. ]+)", cond)
walk["loadavg"] = m.group(1).strip() if m else None
m = re.search(r"trace file closed: (\d+) events", cond)
walk["trace_events_total"] = int(m.group(1)) if m else None
m = re.search(r"(\d+) events?[, ]", cond)
walk["conditions_raw"] = cond[-4000:]

with open(walk_path, "w") as f:
    json.dump(walk, f, indent=2)
print("stamped %s: workload_alive=%s alive=%d dead=%d capture_age_s=%s"
      % (walk_path, walk["workload_alive"], alive, dead, walk["capture_age_s"]))
