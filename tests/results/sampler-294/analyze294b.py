#!/usr/bin/env python3
"""#294 evidence table. Sources, per cell:
   /root/diag294/<tree>-<rate>.log        -- PROC_* (kernel /proc) + METRIC_*
   /root/diag294/<tree>-<rate>.xval2.txt  -- comparator shares + RAW_* ns
A field whose source line is missing prints ABSENT, never 0.
"""
import os, re, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/root/diag294"
rates = [10, 50, 100, 200]
RAWK = ["RAW_WINDOW_S", "RAW_CPU_EXACT_NS", "RAW_CPU_SAMPLED_NS",
        "RAW_AAS_EXACT", "RAW_AAS_SAMPLED",
        "RAW_SAMPLED_CPU_NS_NEEDED_FOR_PARITY", "RAW_SAMPLE_PERIOD_NS",
        "RAW_TOTAL_EXACT_NS", "RAW_TOTAL_SAMPLED_NS"]

def read(tree, rate):
    lg = os.path.join(ROOT, "%s-%d.log" % (tree, rate))
    xv = os.path.join(ROOT, "%s-%d.xval2.txt" % (tree, rate))
    if not (os.path.exists(lg) and os.path.exists(xv)):
        return None
    d = {}
    t = open(lg).read()
    for k in ["PROC_CLIENT_TOTAL_CPU_NS", "PROC_WALL_S"]:
        m = re.search(r"^%s (\S+)" % k, t, re.M)
        d[k] = float(m.group(1)) if m else None
    m = re.search(r"^PROC_DAEMON_PID \d+ TICKS (\d+)", t, re.M)
    d["daemon_ticks"] = int(m.group(1)) if m else None
    if re.search(r"^PROC_PID \d+ GONE", t, re.M):
        d["backend_gone"] = True
    for m in re.finditer(r"^METRIC (\S+) (\S+)", t, re.M):
        d[m.group(1)] = m.group(2)
    x = open(xv).read()
    for k in RAWK:
        m = re.search(r"^%s (\S+)" % k, x, re.M)
        d[k] = float(m.group(1)) if m else None
    m = re.search(r"^CPU\s+(\S+)%\s+(\S+)%\s+(\S+)", x, re.M)
    if m:
        d["ex"], d["sa"], d["pp"] = (float(m.group(i)) for i in (1, 2, 3))
    m = re.search(r"^Max share disagreement.*?: (\S+) pp \((\w+)\)", x, re.M)
    if m:
        d["max_pp"] = float(m.group(1)); d["max_ev"] = m.group(2)
    m = re.search(r"^Top-5 event overlap: (\S+)", x, re.M)
    d["top5"] = m.group(1) if m else None
    m = re.search(r"^RESULT: (\w+)", x, re.M)
    d["result"] = m.group(1) if m else "ABSENT"
    return d

rows = {}
print("CPU share and the kernel cross-check (gate-2, pgbench -c8 -j2, 60 s, PG18.6)")
print("%4s %-5s %7s %9s %6s %8s %7s %6s %14s %13s %10s %11s" % (
    "rate", "tree", "exact%", "sampled%", "pp", "verdict", "top5",
    "AASex", "sampledCPU//proc", "exactCPU//proc", "AASsamp", "noncmd"))
for rate in rates:
    for tree in ("base", "fix"):
        d = read(tree, rate); rows[(tree, rate)] = d
        if not d:
            print("%4d %-5s MISSING" % (rate, tree)); continue
        ps = None
        if d["PROC_CLIENT_TOTAL_CPU_NS"] and d["PROC_WALL_S"] and d["RAW_WINDOW_S"]:
            ps = d["PROC_CLIENT_TOTAL_CPU_NS"] / d["PROC_WALL_S"] * d["RAW_WINDOW_S"]
        d["proc_scaled_ns"] = ps
        f = lambda n: "ABSENT" if (n is None or not ps) else "%.3f" % (n / ps)
        print("%4d %-5s %7.1f %9.1f %6.1f %8s %7s %6.2f %14s %13s %10.2f %11s" % (
            rate, tree, d["ex"], d["sa"], d["pp"], d["result"], d["top5"],
            d["RAW_AAS_EXACT"], f(d["RAW_CPU_SAMPLED_NS"]),
            f(d["RAW_CPU_EXACT_NS"]), d["RAW_AAS_SAMPLED"],
            d.get("noncmd_cpu_samples_total", "ABSENT")))

print()
print("Recheck accounting (fix tree only; the counters do not exist on base)")
print("%4s %8s %10s %12s %9s %12s %14s" % (
    "rate", "at_risk", "recovered", "read_failed", "noncmd",
    "samples", "recovered_ns"))
for rate in rates:
    d = rows[("fix", rate)]
    if not d: continue
    rec = d.get("cmd_gate_order_recovered_total")
    per = d.get("RAW_SAMPLE_PERIOD_NS")
    rns = "ABSENT"
    if rec not in (None, "ABSENT-from-metrics") and per:
        rns = "%.3f Gns" % (int(rec) * per / 1e9)
    print("%4s %8s %10s %12s %9s %12s %14s" % (
        rate, d.get("cmd_gate_order_at_risk_total", "ABSENT"),
        rec, d.get("cmd_gate_order_read_failed_total", "ABSENT"),
        d.get("noncmd_cpu_samples_total", "ABSENT"),
        d.get("samples_total", "ABSENT"), rns))

print()
print("Exact side unmoved? (the fix must not touch it)")
for rate in rates:
    b, f_ = rows[("base", rate)], rows[("fix", rate)]
    if not (b and f_): continue
    pe = lambda a, c: "ABSENT" if (a is None or not c) else "%.3f" % (a / c)
    print("  %4d Hz  exactCPU//proc base=%s fix=%s | AAS_exact base=%.2f fix=%.2f"
          " | exact CPU share base=%.1f%% fix=%.1f%%" % (
        rate, pe(b["RAW_CPU_EXACT_NS"], b["proc_scaled_ns"]),
        pe(f_["RAW_CPU_EXACT_NS"], f_["proc_scaled_ns"]),
        b["RAW_AAS_EXACT"], f_["RAW_AAS_EXACT"], b["ex"], f_["ex"]))

print()
print("ALL FOUR rates against the +/-10 pp rule (not the test's any-one rule):")
for tree in ("base", "fix"):
    v = [("ABSENT" if not rows[(tree, r)] or rows[(tree, r)].get("max_pp") is None
          else ("PASS" if rows[(tree, r)]["max_pp"] <= 10.0 else "FAIL"))
         for r in rates]
    print("  %-5s %s -> %s" % (tree,
          " ".join("%dHz:%s(%.1f)" % (r, s, rows[(tree, r)]["max_pp"])
                   for r, s in zip(rates, v)),
          "ALL FOUR PASS" if all(s == "PASS" for s in v) else "NOT all four"))
for k, d in rows.items():
    if d and d.get("backend_gone"):
        print("WARNING: %s had a backend disappear mid-window; /proc total is a "
              "lower bound for that cell" % (k,))
