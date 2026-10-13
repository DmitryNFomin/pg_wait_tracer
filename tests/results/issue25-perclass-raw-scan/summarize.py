#!/usr/bin/env python3
"""Render measurement.json as the tables REPORT.md quotes (summary.txt)."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "measurement.json")
d = json.load(open(path))
cov = d["coverage"]
print("verdict:", d["verdict"], "exit", d["exit_code"],
      "threshold_ns", d["threshold_ns"])
print("coverage:", {k: cov[k] for k in (
    "files_found", "files_opened", "files_failed", "blocks_total",
    "blocks_decoded", "blocks_failed", "records_walked", "header_records",
    "transitions", "markers", "samples", "wait_records", "cpu_gap_records",
    "cpu_gap_cpu_unknown", "unresolved_records", "expect_transitions",
    "expect_samples")})
print("refusals:", d["refusals"],
      "unmeasured:", d["unmeasured_pure_sleep_classes"])
print()
cls = {c["name"]: c for c in d["classes"]}
print("PURE-SLEEP SET (doc §3 names + the workload's advisory substitute)")
print(f"{'class':22}{'n':>6}{'measured':>9}{'unknown':>8}{'max_cpu_us':>12}"
      f"{'mean_cpu_us':>12}{'median_us':>10}{'p99_us':>9}"
      f"{'max_cpu/dur%':>14}{'mean_cpu/dur%':>14}{'>thr':>5}")
for name in ("Timeout:PgSleep", "Lock:relation", "Lock:advisory",
             "Lock:transactionid", "Lock:extend", "Lock:tuple"):
    c = cls.get(name)
    if not c:
        print(f"{name:22}{0:>6}{0:>9}{0:>8}{'UNMEASURED':>12}{'-':>12}"
              f"{'-':>10}{'-':>9}{'-':>14}{'-':>14}{'-':>5}")
        continue
    print(f"{name:22}{c['count']:>6}{c['measured']:>9}{c['cpu_unknown']:>8}"
          f"{c['max_cpu_ns']/1e3:>12.3f}{c['mean_cpu_ns']/1e3:>12.3f}"
          f"{c['median_cpu_ns']/1e3:>10.3f}{c['p99_cpu_ns']/1e3:>9.3f}"
          f"{c['max_cpu_over_dur']*100:>14.6f}"
          f"{c['mean_cpu_over_dur']*100:>14.6f}{c['over_threshold']:>5}")
print()
print("sleep-class boundary-CPU envelope at full coverage (us):")
for name in ("Timeout:PgSleep", "Lock:relation", "Lock:transactionid",
             "Lock:extend"):
    c = cls.get(name)
    if c:
        print(f"  {name:22} median {c['median_cpu_ns']/1e3:8.3f}"
              f"  p95 {c['p95_cpu_ns']/1e3:8.3f}"
              f"  p99 {c['p99_cpu_ns']/1e3:8.3f}"
              f"  max {c['max_cpu_ns']/1e3:9.3f}")
print()
print("ALL CLASSES")
print(f"{'class':34}{'n':>8}{'meas':>8}{'unk':>6}{'max_cpu_us':>12}"
      f"{'mean_cpu_us':>12}{'max_cpu/dur%':>14}{'>thr':>5}")
for c in sorted(d["classes"], key=lambda c: -c["count"]):
    print(f"{c['name']:34}{c['count']:>8}{c['measured']:>8}"
          f"{c['cpu_unknown']:>6}{c['max_cpu_ns']/1e3:>12.3f}"
          f"{c['mean_cpu_ns']/1e3:>12.3f}{c['max_cpu_over_dur']*100:>14.6f}"
          f"{c['over_threshold']:>5}")
print()
print("pure-sleep crossings:", d["pure_sleep_crossings"],
      " all-class crossings:", d["all_class_crossings"])
for c in d["crossing_events"]:
    e = c["event"]
    ratio = 100.0 * e["cpu_ns"] / e["duration_ns"] if e["duration_ns"] else 0.0
    tag = "SLEEP CROSSING" if e["name"] in ("Timeout:PgSleep", "Lock:relation",
                                           "Lock:advisory") else "other class"
    print(f"  {tag}: {e['name']} pid={e['pid']} cpu_ns={e['cpu_ns']} "
          f"dur_ns={e['duration_ns']} ratio={ratio:.6f}% "
          f"ts_wall_ns={e['ts_wall_ns']} query_id={e['query_id']} "
          f"block={e['block']} record={e['record']}")
