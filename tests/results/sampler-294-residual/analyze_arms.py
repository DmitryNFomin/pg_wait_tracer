#!/usr/bin/env python3
"""analyze_arms.py RESULTS_DIR — tabulate the #294 arm A/B/C runs.

Reads each arm's xval.txt (cross_validate --raw-ns --show-idle) and sched.txt
and prints one row per run plus a per-arm summary.

Every field is REQUIRED. A run missing any of them is reported as BROKEN and
excluded from the arm's summary with a named reason -- it is never silently
treated as a zero, and an arm that loses runs says how many it has left, so a
summary over n=1 cannot be mistaken for one over n=3.
"""
import os, re, sys, statistics

RESULTS = sys.argv[1] if len(sys.argv) > 1 else "."
ARMS = ["A", "B", "C"]
REPS = [1, 2, 3]
NEEDED = ["RAW_TOTAL_EXACT_NS", "RAW_TOTAL_SAMPLED_NS",
          "RAW_CPU_EXACT_NS", "RAW_CPU_SAMPLED_NS",
          "RAW_WINDOW_S", "RAW_CPU_SAMPLES", "RAW_NONCPU_SAMPLES",
          "IDLE_TOTAL_EXACT_NS", "IDLE_TOTAL_SAMPLED_NS", "IDLE_SAMPLES"]


def read_raw(path):
    out = {}
    if not os.path.exists(path):
        return None, "xval.txt missing"
    for line in open(path):
        m = re.match(r"^(RAW_[A-Z_]+|IDLE_[A-Z_]+|IDLE_PACING_MASK) (\S+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    if "IDLE-UNAVAILABLE" in open(path).read():
        return None, "cross_validate REFUSED the idle breakdown (empty pacing mask)"
    missing = [k for k in NEEDED if k not in out]
    if missing:
        return None, "fields absent: " + ",".join(missing)
    return out, None


def shares(path):
    """The per-event share table, both tiers, as printed."""
    rows, started = [], False
    for line in open(path):
        if line.startswith("wait_event "):
            started = True; continue
        if started:
            if line.startswith("---"):
                continue
            if not line.strip() or line.startswith("Samples in window"):
                break
            p = line.split()
            if len(p) >= 4 and p[1].endswith("%"):
                rows.append((p[0], float(p[1].rstrip("%")), float(p[2].rstrip("%"))))
    return rows


runs = {a: [] for a in ARMS}
broken = []
print("=" * 108)
print("%-5s %-4s %14s %14s %7s %8s  %12s %12s %8s %7s" % (
    "arm", "rep", "cpu_exact_ns", "cpu_sampled_ns", "ratio", "delta_pp",
    "tot_exact_ns", "tot_sampl_ns", "policy", "tps"))
print("=" * 108)
for rep in REPS:
    for arm in ARMS:
        d = os.path.join(RESULTS, "arm%s-r%d" % (arm, rep))
        raw, err = read_raw(os.path.join(d, "xval.txt"))
        if raw is None:
            broken.append((arm, rep, err))
            print("%-5s %-4s BROKEN: %s" % (arm, rep, err))
            continue
        ce = float(raw["RAW_CPU_EXACT_NS"]); cs = float(raw["RAW_CPU_SAMPLED_NS"])
        te = float(raw["RAW_TOTAL_EXACT_NS"]); ts = float(raw["RAW_TOTAL_SAMPLED_NS"])
        if ce <= 0 or te <= 0 or ts <= 0:
            broken.append((arm, rep, "non-positive total (ce=%g te=%g ts=%g)" % (ce, te, ts)))
            print("%-5s %-4s BROKEN: non-positive total" % (arm, rep))
            continue
        ratio = cs / ce
        # Signed pp delta on the CPU SHARE: sampled share - exact share.
        # Negative = the sampled tier reads LESS CPU, which is the #294 sign.
        delta = 100.0 * cs / ts - 100.0 * ce / te
        pol = "?"
        sp = os.path.join(d, "sched.txt")
        if os.path.exists(sp):
            m = re.search(r"policy=(\d+)", open(sp).read())
            if m: pol = {"0": "OTHER", "1": "FIFO"}.get(m.group(1), m.group(1))
        tps = "?"
        pb = os.path.join(d, "pgbench.txt")
        if os.path.exists(pb):
            m = re.search(r"^tps = ([\d.]+)", open(pb).read(), re.M)
            if m: tps = "%.0f" % float(m.group(1))
        runs[arm].append(dict(rep=rep, ratio=ratio, delta=delta, ce=ce, cs=cs,
                              te=te, ts=ts, pol=pol, raw=raw, dir=d))
        print("%-5s %-4s %14.0f %14.0f %7.4f %+8.2f  %12.0f %12.0f %8s %7s" % (
            arm, rep, ce, cs, ratio, delta, te, ts, pol, tps))

print()
print("=" * 108)
print("PER-ARM SUMMARY  (ratio = sampled CPU ns / exact CPU ns; "
      "delta_pp = sampled CPU share - exact CPU share, signed)")
print("=" * 108)
labels = {"A": "A baseline      CLIENTS=8 SCHED_OTHER",
          "B": "B real-time     CLIENTS=8 SCHED_FIFO 10",
          "C": "C unsaturated   CLIENTS=3 SCHED_OTHER"}
for arm in ARMS:
    rs = runs[arm]
    if not rs:
        print("%-42s n=0  NO USABLE RUN" % labels[arm]); continue
    ratios = [r["ratio"] for r in rs]; deltas = [r["delta"] for r in rs]
    pols = sorted({r["pol"] for r in rs})
    print("%-42s n=%d  ratio median %.4f  [min %.4f max %.4f spread %.4f]"
          % (labels[arm], len(rs), statistics.median(ratios),
             min(ratios), max(ratios), max(ratios) - min(ratios)))
    print("%-42s      delta_pp median %+.2f  [min %+.2f max %+.2f spread %.2f]  policy=%s"
          % ("", statistics.median(deltas), min(deltas), max(deltas),
             max(deltas) - min(deltas), ",".join(pols)))
    print("%-42s      per-run ratio %s" % ("", " ".join("%.4f" % x for x in ratios)))
    print("%-42s      per-run delta %s" % ("", " ".join("%+.2f" % x for x in deltas)))

if broken:
    print()
    print("BROKEN RUNS (excluded, with reason — never counted as zero):")
    for a, r, e in broken:
        print("  arm%s rep%d: %s" % (a, r, e))

print()
print("=" * 108)
print("IDLE (NOT DB Time) — the destination the shares cannot show")
print("=" * 108)
print("%-5s %-4s %16s %16s %8s %12s" % ("arm", "rep", "idle_exact_ns",
                                        "idle_sampled_ns", "idle_sm", "mask"))
for arm in ARMS:
    for r in runs[arm]:
        raw = r["raw"]
        print("%-5s %-4s %16.0f %16.0f %8s %12s" % (
            arm, r["rep"], float(raw["IDLE_TOTAL_EXACT_NS"]),
            float(raw["IDLE_TOTAL_SAMPLED_NS"]), raw["IDLE_SAMPLES"],
            raw.get("IDLE_PACING_MASK", "ABSENT")))

print()
print("=" * 108)
print("PER-EVENT SHARE TABLE, BOTH TIERS (first rep of each arm; "
      "events >= 0.5%% exact share)")
print("=" * 108)
for arm in ARMS:
    if not runs[arm]:
        continue
    r = runs[arm][0]
    print("\n--- arm %s rep %d (%s) ---" % (arm, r["rep"], r["dir"]))
    print("%-28s %12s %12s %10s" % ("wait_event", "exact%", "sampled%", "delta_pp"))
    for name, ex, sm in shares(os.path.join(r["dir"], "xval.txt")):
        if ex >= 0.5 or sm >= 0.5:
            print("%-28s %11.1f%% %11.1f%% %+9.1f" % (name, ex, sm, sm - ex))
