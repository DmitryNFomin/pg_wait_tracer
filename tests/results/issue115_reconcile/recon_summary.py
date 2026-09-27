#!/usr/bin/env python3
"""recon_summary.py <dir> — one reconciled table from the recon/*.txt runs.

For each configuration (sat / unsat) and each quantity, prints
min..max across the repeats and the mean, so no single run can carry a claim.
"""
import sys, os, re, glob, collections, statistics

d = sys.argv[1]
runs = collections.defaultdict(dict)   # label -> {key: value}

pat = {
    "win_s":      re.compile(r"^WINDOW \[\d+,\d+\) secs=([\d.]+)"),
    "tps":        re.compile(r"tps = ([\d.]+)"),
}


def add(lbl, k, v):
    runs[lbl][k] = v


for f in sorted(glob.glob(os.path.join(d, "*.txt"))):
    lbl = os.path.basename(f)[:-4]
    sec = None
    for line in open(f):
        m = pat["win_s"].match(line)
        if m:
            add(lbl, "win_s", float(m.group(1)))
        m = pat["tps"].search(line)
        if m and "tps" not in runs[lbl]:
            add(lbl, "tps", float(m.group(1)))
        if line.startswith("--- M2 exact side"):
            sec = "M2"
        elif line.startswith("--- M1 exact side"):
            sec = "M1"
        elif line.startswith("--- reference"):
            sec = "ref"
        elif line.startswith("--- kernel"):
            sec = "kern"
        if sec in ("M1", "M2"):
            m = re.match(r"^SAMPLED .*cpu=([\d.]+)s nonidle=([\d.]+)s\s+share=([\d.]+)%", line)
            if m:
                add(lbl, f"{sec}_sampled_share", float(m.group(3)))
                add(lbl, f"{sec}_sampled_cpu_s", float(m.group(1)))
            m = re.match(r"^EXACT (\w) \([^)]*\)\s+cpu=([\d.]+)s wait=([\d.]+)s\s+share=([\d.]+)%", line)
            if m:
                add(lbl, f"{sec}_exact{m.group(1)}_share", float(m.group(4)))
                add(lbl, f"{sec}_exact{m.group(1)}_cpu_s", float(m.group(2)))
                add(lbl, f"{sec}_wait_s", float(m.group(3)))
            m = re.match(r"^CMD-OPEN total = ([\d.]+) s over (\d+) commands.*cpu outside cmd = ([\d.]+) s", line)
            if m:
                add(lbl, f"{sec}_cmdopen_s", float(m.group(1)))
                add(lbl, f"{sec}_ncmd", float(m.group(2)))
                add(lbl, f"{sec}_cpu_out_cmd_s", float(m.group(3)))
    for line in open(f):
        m = re.match(r"^PGSA_(V\d) (\S+)\s+share=\s*([\d.]+)%\s+cpu_s=\s*([\d.]+) nonidle_s=\s*([\d.]+)", line)
        if m:
            add(lbl, f"pgsa_{m.group(2)}_{m.group(1)}_share", float(m.group(3)))
            add(lbl, f"pgsa_{m.group(2)}_{m.group(1)}_cpu_s", float(m.group(4)))
        m = re.match(r"^PGSA_IN (\S+)\s+.*act_cpu=(\d+) act_wait=(\d+) torn=(\d+)\(([\d.]+)%\) "
                     r"nonactive_nowait=(\d+)\(([\d.]+)%\)", line)
        if m and m.group(1) == "M2strict":
            ac, aw, tn, nw = (int(m.group(2)), int(m.group(3)), int(m.group(4)),
                              int(m.group(6)))
            add(lbl, "torn_pct_of_all_rows", float(m.group(5)))
            add(lbl, "torn_pct_of_active_rows", 100.0 * tn / (ac + aw + tn))
            add(lbl, "nonactive_nowait_pct_of_all_rows", float(m.group(7)))
            add(lbl, "pgsa_active_frac_pct", 100.0 * (ac + aw) / (ac + aw + tn + nw) if (ac+aw+tn+nw) else 0)
        m = re.match(r"^ORACLE .*share=([\d.]+)%", line)
        if m:
            add(lbl, "oracle_py_share", float(m.group(1)))
        m = re.match(r"\s+schedstat f1 .*=\s+([\d.]+) s", line)
        if m:
            add(lbl, "kern_exec_s", float(m.group(1)))
        m = re.match(r"\s+schedstat f2 .*=\s+([\d.]+) s", line)
        if m:
            add(lbl, "kern_delay_s", float(m.group(1)))
        m = re.match(r"\s+on-CPU\s+\(executing\)\s+=\s+([\d.]+) s", line)
        if m:
            add(lbl, "sched_on_s", float(m.group(1)))
        m = re.match(r"\s+run-queue delay .*=\s+([\d.]+) s", line)
        if m:
            add(lbl, "sched_rq_s", float(m.group(1)))
        m = re.match(r"\s+off-CPU .*=\s+([\d.]+) s", line)
        if m:
            add(lbl, "sched_off_s", float(m.group(1)))
        m = re.match(r"\s+SCHEDULING BIAS = (-?[\d.]+) pp", line)
        if m:
            add(lbl, "sched_bias_pp", float(m.group(1)))
        m = re.match(r"\s+by time\s+= ([\d.]+)%", line)
        if m:
            add(lbl, "onrq_by_time_pct", float(m.group(1)))
        m = re.match(r"\s+at sampler switch-ins\s+= ([\d.]+)%", line)
        if m:
            add(lbl, "onrq_by_sampler_pct", float(m.group(1)))

# derived per run
for lbl, r in runs.items():
    if "M2_exactS_share" in r and "M2_exactC_share" in r:
        r["straddle_pp"] = r["M2_exactS_share"] - r["M2_exactC_share"]
    if "M2_exactS_share" in r and "pgsa_M2strict_V1_share" in r:
        r["S_minus_pgsaV1_pp"] = r["M2_exactS_share"] - r["pgsa_M2strict_V1_share"]
    if "M2_exactC_share" in r and "pgsa_M2strict_V1_share" in r:
        r["C_minus_pgsaV1_pp"] = r["M2_exactC_share"] - r["pgsa_M2strict_V1_share"]
    if "M2_exactC_share" in r and "pgsa_M2strict_V2_share" in r:
        r["C_minus_pgsaV2_pp"] = r["M2_exactC_share"] - r["pgsa_M2strict_V2_share"]
    if "pgsa_M2strict_V2_share" in r and "pgsa_M2strict_V1_share" in r:
        r["classifier_V2_minus_V1_pp"] = r["pgsa_M2strict_V2_share"] - r["pgsa_M2strict_V1_share"]
    if "M2_exactS_share" in r and "M1_exactS_share" in r:
        r["backendset_M2_minus_M1_pp"] = r["M2_exactS_share"] - r["M1_exactS_share"]
    if "pgsa_M2strict_V1_share" in r and "pgsa_M1all_V1_share" in r:
        r["pgsa_backendset_pp"] = r["pgsa_M2strict_V1_share"] - r["pgsa_M1all_V1_share"]
    if "kern_exec_s" in r and "kern_delay_s" in r and "M2_wait_s" in r:
        onrq = r["kern_exec_s"] + r["kern_delay_s"]
        r["kernel_cpu_share_f1f2"] = 100.0 * onrq / (onrq + r["M2_wait_s"])
        r["kernel_cpu_share_f1"] = 100.0 * r["kern_exec_s"] / (r["kern_exec_s"] + r["M2_wait_s"])
    if "M2_exactS_cpu_s" in r and "kern_exec_s" in r and "kern_delay_s" in r:
        r["containment_exactS_over_f1f2"] = 100.0 * r["M2_exactS_cpu_s"] / (r["kern_exec_s"] + r["kern_delay_s"])
    if "M2_ncmd" in r and "win_s" in r:
        r["cmds_per_s"] = r["M2_ncmd"] / r["win_s"]
    # "whole backend" definition: no-wait time INSIDE and OUTSIDE a command.
    # This is the definition pg_stat_activity's V2 and the kernel's f1+f2
    # answer; the exact tier's own share (and V1) is the in-command one.
    if "M2_exactC_cpu_s" in r and "M2_cpu_out_cmd_s" in r and "M2_wait_s" in r:
        c = r["M2_exactC_cpu_s"] + r["M2_cpu_out_cmd_s"]
        r["exact_whole_backend_share"] = 100.0 * c / (c + r["M2_wait_s"])
    # inv-1 expressed the sampler bias in pp of the CPU share of non-idle;
    # inv-2 measured it in pp of wall-clock on-cpu-or-runnable. Convert
    # inv-2's directly measured figure into inv-1's units.
    if "sched_bias_pp" in r and "pgsa_active_frac_pct" in r and r["pgsa_active_frac_pct"]:
        r["sched_bias_pp_in_share_units"] = r["sched_bias_pp"] / (r["pgsa_active_frac_pct"] / 100.0)

cfgs = collections.defaultdict(list)
for lbl in sorted(runs):
    cfgs[re.sub(r"\d+$", "", lbl)].append(lbl)

keys = [k for k in dict.fromkeys(k for r in runs.values() for k in r)]
order = ["win_s", "tps", "cmds_per_s",
         "M2_exactS_share", "M2_exactA_share", "M2_exactB_share", "M2_exactC_share",
         "M2_sampled_share",
         "pgsa_M2strict_V1_share", "pgsa_M2strict_V2_share", "pgsa_M2strict_V3_share",
         "pgsa_M2strict_V4_share", "oracle_py_share",
         "exact_whole_backend_share",
         "kernel_cpu_share_f1", "kernel_cpu_share_f1f2",
         "kern_exec_s", "kern_delay_s", "sched_on_s", "sched_rq_s", "sched_off_s",
         "M2_wait_s", "M2_cmdopen_s", "M2_cpu_out_cmd_s",
         "straddle_pp", "S_minus_pgsaV1_pp", "C_minus_pgsaV1_pp", "C_minus_pgsaV2_pp",
         "classifier_V2_minus_V1_pp",
         "backendset_M2_minus_M1_pp", "pgsa_backendset_pp",
         "onrq_by_time_pct", "onrq_by_sampler_pct", "sched_bias_pp",
         "sched_bias_pp_in_share_units",
         "torn_pct_of_all_rows", "torn_pct_of_active_rows",
         "nonactive_nowait_pct_of_all_rows", "pgsa_active_frac_pct",
         "containment_exactS_over_f1f2"]
order += [k for k in keys if k not in order]

cfgnames = sorted(cfgs)
w = 24
print(f"{'quantity':<34s}" + "".join(f"{c:>{w}s}" for c in cfgnames))
print("-" * (34 + w * len(cfgnames)))
for k in order:
    cells = []
    any_ = False
    for c in cfgnames:
        vals = [runs[l][k] for l in cfgs[c] if k in runs[l]]
        if not vals:
            cells.append(f"{'-':>{w}s}")
            continue
        any_ = True
        mean = statistics.fmean(vals)
        cells.append(f"{mean:>10.2f} [{min(vals):.2f}..{max(vals):.2f}]".rjust(w))
    if any_:
        print(f"{k:<34s}" + "".join(cells))
print()
print("n repeats:", {c: len(cfgs[c]) for c in cfgnames})
