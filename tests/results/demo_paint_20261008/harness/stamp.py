#!/usr/bin/env python3
"""Merge a run's box-side conditions into its walk JSON.

workload_alive gates the whole walk: paint_analyse.py REFUSES a walk whose
liveness is not confirmed, because paint measured against an idle system is
not the demo's paint. So this field must never be a guess.

THE BUG THIS FILE NOW DOCUMENTS (2026-10-08). The 2026-10-07 version read

    walk["workload_alive"] = (alive == 3 and dead == 0)

-- a hardcoded 3, matching the number of processes box_run_conditions.sh
checked at the time (pgbench, the Python workload, the daemon). Adding the
burst tier made that four, so the FIRST paint run of 2026-10-08 recorded
`workload_alive=False` while its own conditions file listed four alive
processes and zero dead ones, and paint_analyse.py refused a walk that was
in fact perfectly valid. A magic number coupled by convention to another
script's loop body is exactly the kind of gate that stops being able to see
and nobody notices, because its failure direction here happens to be safe
(refuse) -- the same mistake with the comparison the other way round
(`alive >= 3`) would have APPROVED a walk with the burst tier dead.

So the expected count is no longer guessed. box_run_conditions.sh now prints

    procs_expected=N

from the length of its OWN process list, and this script requires
`alive == procs_expected and dead == 0`. A conditions file without that line
(every run before 2026-10-08) falls back to `alive >= MIN_EXPECTED_PROCS and
dead == 0`, which still refuses a missing or dead tier but cannot notice a
tier that was silently dropped from the conditions script itself. Which rule
was applied is recorded in the walk JSON as `workload_alive_basis`, so a
reviewer never has to infer it.

An unreadable conditions file leaves workload_alive False -- an absent answer
is not a yes.
"""
import json
import re
import sys

# Floor for the legacy fallback: pgbench + live_loop_workload.py +
# burst_load.sh + the daemon. A floor, not an equality, because the fallback
# exists for files written before the count was emitted; its job is to refuse
# zero-to-three, not to validate a list it cannot see the length of.
MIN_EXPECTED_PROCS = 4

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

# Two separate questions, deliberately: is there a procs_expected LINE at all,
# and does its value parse? Matching only `=(\d+)` conflates them -- a line
# reading `procs_expected=abc` then looks identical to no line at all and falls
# through to the permissive legacy branch, which approves. Caught by
# test_stamp.sh's "procs_expected unparseable" case on its first run.
line = re.search(r"^procs_expected=(.*)$", cond, re.M)
m = re.search(r"^procs_expected=(\d+)\s*$", cond, re.M)
if line is not None and m is None:
    expected = None
    ok = False
    basis = ("procs_expected=%r does not parse as a count -- refusing rather "
             "than falling back to the legacy rule, which would approve"
             % line.group(1).strip())
elif m:
    expected = int(m.group(1))
    # expected must itself be sane: a conditions script that reported
    # procs_expected=0 would otherwise make `alive == expected` true with
    # nothing checked at all.
    ok = (expected >= MIN_EXPECTED_PROCS and alive == expected and dead == 0)
    basis = ("procs_expected=%d (alive=%d dead=%d)" % (expected, alive, dead))
elif cond:
    expected = None
    ok = (alive >= MIN_EXPECTED_PROCS and dead == 0)
    basis = ("legacy fallback: no procs_expected line in the conditions file; "
             "required alive >= %d and dead == 0 (alive=%d dead=%d)"
             % (MIN_EXPECTED_PROCS, alive, dead))
else:
    expected = None
    ok = False
    basis = "conditions file unreadable -- an absent answer is not a yes"

walk["workload_alive"] = ok
walk["workload_alive_basis"] = basis
walk["conditions_alive_count"] = alive
walk["conditions_dead_count"] = dead
walk["conditions_procs_expected"] = expected

m = re.search(r"capture_age_s=(\d+)", cond)
walk["capture_age_s"] = int(m.group(1)) if m else None
m = re.search(r"loadavg=([\d. ]+)", cond)
walk["loadavg"] = m.group(1).strip() if m else None
m = re.search(r"trace file closed: (\d+) events", cond)
walk["trace_events_total"] = int(m.group(1)) if m else None
walk["conditions_raw"] = cond[-4000:]

with open(walk_path, "w") as f:
    json.dump(walk, f, indent=2)
print("stamped %s: workload_alive=%s (%s) capture_age_s=%s"
      % (walk_path, walk["workload_alive"], basis, walk["capture_age_s"]))
