#!/usr/bin/env python3
"""deltas.py RESULTS_DIR — per-class sampled-minus-exact deltas and the health
counters, for all nine #294 arm runs. Every field is required; an absent one
prints ABSENT, never 0."""
import os, re, sys

R = sys.argv[1]
print("=== per-class deltas, sampled minus exact (Gns = 1e9 ns) ===")
print("%-6s %10s %10s %10s %10s %10s %10s" % (
    "run", "dCPU_Gns", "dWait_Gns", "dIdle_Gns", "cpu_smpl", "wait_smpl", "idle_smpl"))
for a in "ABC":
    for r in (1, 2, 3):
        p = os.path.join(R, "arm%s-r%d" % (a, r), "xval.txt")
        if not os.path.exists(p):
            print("%-6s MISSING xval.txt" % (a + str(r))); continue
        d = {}
        for line in open(p):
            m = re.match(r"^(RAW_[A-Z_]+|IDLE_[A-Z_]+) (\S+)", line)
            if m:
                d[m.group(1)] = m.group(2)
        need = ["RAW_CPU_SAMPLED_NS", "RAW_CPU_EXACT_NS", "RAW_NONCPU_SAMPLED_NS",
                "RAW_NONCPU_EXACT_NS", "IDLE_TOTAL_SAMPLED_NS",
                "IDLE_TOTAL_EXACT_NS", "RAW_CPU_SAMPLES", "RAW_NONCPU_SAMPLES",
                "IDLE_SAMPLES"]
        miss = [k for k in need if k not in d]
        if miss:
            print("%-6s ABSENT: %s" % (a + str(r), ",".join(miss))); continue
        g = lambda k: float(d[k]) / 1e9
        print("%-6s %+10.1f %+10.1f %+10.1f %10s %10s %10s" % (
            a + str(r),
            g("RAW_CPU_SAMPLED_NS") - g("RAW_CPU_EXACT_NS"),
            g("RAW_NONCPU_SAMPLED_NS") - g("RAW_NONCPU_EXACT_NS"),
            g("IDLE_TOTAL_SAMPLED_NS") - g("IDLE_TOTAL_EXACT_NS"),
            d["RAW_CPU_SAMPLES"], d["RAW_NONCPU_SAMPLES"], d["IDLE_SAMPLES"]))

print()
print("=== health counters (a nonzero read-failure/invalid count would mean "
      "something IS being dropped, which the issue states is not the case) ===")
keys = ["samples_total", "sampled_attr_tick_read_failures_total",
        "invalid_wait_reads_total", "cmd_gate_order_read_failed_total",
        "cmd_gate_order_at_risk_total", "cmd_gate_order_recovered_total",
        "noncmd_cpu_samples_total", "sampled_idle_in_command_total"]
for a in "ABC":
    for r in (1, 2, 3):
        p = os.path.join(R, "arm%s-r%d.log" % (a, r))
        if not os.path.exists(p):
            print("%-6s MISSING log" % (a + str(r))); continue
        txt = open(p).read()
        vals = []
        for k in keys:
            m = re.search(r"^METRIC %s (\S+)$" % k, txt, re.M)
            vals.append("%s=%s" % (k.replace("_total", ""),
                                   m.group(1) if m else "ABSENT"))
        print("%-6s %s" % (a + str(r), " ".join(vals)))
