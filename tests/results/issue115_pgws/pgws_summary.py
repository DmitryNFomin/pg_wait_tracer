#!/usr/bin/env python3
"""pgws_summary.py <dir> — one table from the pgws/*.txt runs.

Same shape as recon_summary.py: per configuration (sat / unsat), mean and
min..max across the repeats, so no single run carries a claim.
"""
import sys, os, re, glob, collections, statistics

d = sys.argv[1]
runs = collections.defaultdict(dict)


def add(lbl, k, v):
    runs[lbl][k] = v


for f in sorted(glob.glob(os.path.join(d, "*.txt"))):
    lbl = os.path.basename(f)[:-4]
    sec = None
    bias_tag = None
    for line in open(f):
        if line.startswith("--- "):
            sec = line.strip()
        m = re.match(r"^WINDOW \[\d+,\d+\) secs=([\d.]+)", line)
        if m:
            add(lbl, "win_s", float(m.group(1)))
        m = re.search(r"tps = ([\d.]+)", line)
        if m and "tps" not in runs[lbl]:
            add(lbl, "tps", float(m.group(1)))

        # ---- our tiers -------------------------------------------------
        m = re.match(r"^SAMPLED .*cpu=([\d.]+)s nonidle=([\d.]+)s\s+share=([\d.]+)%", line)
        if m:
            add(lbl, "sampled_share", float(m.group(3)))
            add(lbl, "sampled_cpu_s", float(m.group(1)))
        m = re.match(r"^EXACT (\w) \([^)]*\)\s+cpu=([\d.]+)s wait=([\d.]+)s\s+share=([\d.]+)%", line)
        if m:
            add(lbl, f"exact{m.group(1)}_share", float(m.group(4)))
            add(lbl, f"exact{m.group(1)}_cpu_s", float(m.group(2)))
            add(lbl, "wait_s", float(m.group(3)))
        m = re.match(r"^CMD-OPEN total = ([\d.]+) s over (\d+) commands.*cpu outside cmd = ([\d.]+) s", line)
        if m:
            add(lbl, "cmdopen_s", float(m.group(1)))
            add(lbl, "ncmd", float(m.group(2)))
            add(lbl, "cpu_out_cmd_s", float(m.group(3)))

        # ---- pg_stat_activity ------------------------------------------
        m = re.match(r"^PGSA_(V\d) (\S+)\s+share=\s*([\d.]+)%\s+cpu_s=\s*([\d.]+) nonidle_s=\s*([\d.]+)", line)
        if m:
            add(lbl, f"pgsa_{m.group(1)}_share", float(m.group(3)))
            add(lbl, f"pgsa_{m.group(1)}_cpu_s", float(m.group(4)))
        m = re.match(r"^PGSA_IN \S+\s+iters=(\d+) \(([\d.]+) Hz\).*act_cpu=(\d+) act_wait=(\d+) "
                     r"torn=(\d+)\(([\d.]+)%\) nonactive_nowait=(\d+)\(([\d.]+)%\)", line)
        if m:
            ac, aw, tn, nw = (int(m.group(3)), int(m.group(4)), int(m.group(5)),
                              int(m.group(7)))
            add(lbl, "pgsa_rate_hz", float(m.group(2)))
            add(lbl, "torn_pct_of_all_rows", float(m.group(6)))
            add(lbl, "nonactive_nowait_pct_of_all_rows", float(m.group(8)))
            tot = ac + aw + tn + nw
            add(lbl, "pgsa_active_frac_pct", 100.0 * (ac + aw) / tot if tot else 0)

        # ---- pg_wait_sampling -------------------------------------------
        m = re.match(r"^PGWS_IN \S+\s+snaps=(\d+) span=([\d.]+)s .*misalign ([\d.]+)s\) "
                     r"backends=(\d+) probes=(\d+) eff_rate=([\d.]+)Hz "
                     r"cpu_n=(\d+) wait_n=(\d+) idle_clientread_n=(\d+)", line)
        if m:
            cpu_n, wait_n, idle_n = int(m.group(7)), int(m.group(8)), int(m.group(9))
            add(lbl, "pgws_span_misalign_s", float(m.group(3)))
            add(lbl, "pgws_eff_rate_hz", float(m.group(6)))
            tot = cpu_n + wait_n + idle_n
            add(lbl, "pgws_nonidle_frac_pct", 100.0 * (cpu_n + wait_n) / tot if tot else 0)
        m = re.match(r"^PGWS\s+\S+\s+share=\s*([\d.]+)%\s+cpu_s=\s*([\d.]+) nonidle_s=\s*([\d.]+)", line)
        if m:
            add(lbl, "pgws_share", float(m.group(1)))
            add(lbl, "pgws_cpu_s", float(m.group(2)))

        # ---- kernel ------------------------------------------------------
        if sec and "collector overhead" in sec:
            m = re.match(r"\s+schedstat f1 .*=\s+([\d.]+) s", line)
            if m:
                add(lbl, "collector_kern_exec_s", float(m.group(1)))
            m = re.match(r"\s+schedstat f2 .*=\s+([\d.]+) s", line)
            if m:
                add(lbl, "collector_kern_delay_s", float(m.group(1)))
        else:
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

        # ---- scheduling bias, per observer --------------------------------
        m = re.match(r"^SAMPBIAS\[(\w+)\]", line)
        if m:
            bias_tag = m.group(1)
        m = re.match(r"\s+by time\s+= ([\d.]+)%", line)
        if m and bias_tag:
            add(lbl, f"onrq_by_time_pct", float(m.group(1)))
        m = re.match(r"\s+at sampler switch-ins\s+= ([\d.]+)%", line)
        if m and bias_tag:
            add(lbl, f"onrq_at_{bias_tag}_pct", float(m.group(1)))
        m = re.match(r"\s+SCHEDULING BIAS\[(\w+)\] = (-?[\d.]+) pp", line)
        if m:
            add(lbl, f"bias_{m.group(1)}_pp", float(m.group(2)))

        # ---- collector scheduling detail ----------------------------------
        m = re.match(r"^(CDELAY|SDELAY|CBURST)_US n=(\d+) mean=([\d.]+)us "
                     r"p50=\[([^,]+),([^)]+)\) p90=\[([^,]+),([^)]+)\)", line)
        if m:
            k = m.group(1).lower()
            add(lbl, f"{k}_mean_us", float(m.group(3)))
            add(lbl, f"{k}_p50_lo_us", float(m.group(4).replace("K", "e3").replace("M", "e6")))
            add(lbl, f"{k}_p90_lo_us", float(m.group(6).replace("K", "e3").replace("M", "e6")))
        m = re.match(r"^COLLECTOR wakeups=(\d+) \(([\d.]+)/s\)\s+switch-ins=(\d+) \(([\d.]+)/s\)"
                     r"\s+wakeup-triggered switch-ins=(\d+) \(([\d.]+)/s\)", line)
        if m:
            add(lbl, "collector_wakeups_hz", float(m.group(2)))
            add(lbl, "collector_switchins_hz", float(m.group(4)))
            add(lbl, "collector_probes_hz", float(m.group(6)))
        m = re.match(r"^COLLECTOR on-CPU=([\d.]+)s = ([\d.]+)%", line)
        if m:
            add(lbl, "collector_oncpu_pct_of_1cpu", float(m.group(2)))
        m = re.match(r"^PGSA_SAMPLER on-CPU=([\d.]+)s = ([\d.]+)%", line)
        if m:
            add(lbl, "pgsa_sampler_oncpu_pct_of_1cpu", float(m.group(2)))

# ---- derived ----------------------------------------------------------
for lbl, r in runs.items():
    if "exactS_share" in r and "exactC_share" in r:
        r["straddle_pp"] = r["exactS_share"] - r["exactC_share"]
    if "kern_exec_s" in r and "kern_delay_s" in r and "wait_s" in r:
        onrq = r["kern_exec_s"] + r["kern_delay_s"]
        r["kernel_cpu_share_f1f2"] = 100.0 * onrq / (onrq + r["wait_s"])
        r["kernel_cpu_share_f1"] = 100.0 * r["kern_exec_s"] / (r["kern_exec_s"] + r["wait_s"])
    if "ncmd" in r and "win_s" in r:
        r["cmds_per_s"] = r["ncmd"] / r["win_s"]
    # whole-backend definition: no-wait time inside AND outside a command --
    # the definition pg_stat_activity V2, pg_wait_sampling and the kernel's
    # f1+f2 all answer. The tiers' own share is the in-command one.
    if "exactC_cpu_s" in r and "cpu_out_cmd_s" in r and "wait_s" in r:
        c = r["exactC_cpu_s"] + r["cpu_out_cmd_s"]
        r["exact_whole_backend_share"] = 100.0 * c / (c + r["wait_s"])
    # bias in pp of the CPU SHARE = bias in pp of wall / that observer's own
    # non-idle fraction of wall (its denominator).
    if "bias_pgsa_pp" in r and r.get("pgsa_active_frac_pct"):
        r["bias_pgsa_pp_share_units"] = r["bias_pgsa_pp"] / (r["pgsa_active_frac_pct"] / 100.0)
    for tag in ("pgws_all", "pgws_probe"):
        if f"bias_{tag}_pp" in r and r.get("pgws_nonidle_frac_pct"):
            r[f"bias_{tag}_pp_share_units"] = r[f"bias_{tag}_pp"] / (r["pgws_nonidle_frac_pct"] / 100.0)
    # three-way, all on the WHOLE-BACKEND definition
    if "pgws_share" in r and "exact_whole_backend_share" in r:
        r["pgws_minus_exactWB_pp"] = r["pgws_share"] - r["exact_whole_backend_share"]
    if "pgws_share" in r and "kernel_cpu_share_f1f2" in r:
        r["pgws_minus_kernel_pp"] = r["pgws_share"] - r["kernel_cpu_share_f1f2"]
    if "pgws_share" in r and "pgsa_V2_share" in r:
        r["pgws_minus_pgsaV2_pp"] = r["pgws_share"] - r["pgsa_V2_share"]
    if "pgsa_V2_share" in r and "exact_whole_backend_share" in r:
        r["pgsaV2_minus_exactWB_pp"] = r["pgsa_V2_share"] - r["exact_whole_backend_share"]
    if "pgsa_V2_share" in r and "kernel_cpu_share_f1f2" in r:
        r["pgsaV2_minus_kernel_pp"] = r["pgsa_V2_share"] - r["kernel_cpu_share_f1f2"]
    if "exact_whole_backend_share" in r and "kernel_cpu_share_f1f2" in r:
        r["exactWB_minus_kernel_pp"] = r["exact_whole_backend_share"] - r["kernel_cpu_share_f1f2"]
    if "sched_on_s" in r and "sched_rq_s" in r and (r["sched_on_s"] + r["sched_rq_s"]):
        r["rq_frac_of_onrq_pct"] = 100.0 * r["sched_rq_s"] / (r["sched_on_s"] + r["sched_rq_s"])

cfgs = collections.defaultdict(list)
for lbl in sorted(runs):
    cfgs[re.sub(r"\d+$", "", lbl)].append(lbl)

order = [
    "win_s", "tps", "cmds_per_s", "rq_frac_of_onrq_pct",
    "--- CPU share, IN-COMMAND definition (what the tiers report) ---",
    "exactS_share", "exactC_share", "sampled_share", "pgsa_V1_share",
    "--- CPU share, WHOLE-BACKEND definition (what pg_wait_sampling reports) ---",
    "exact_whole_backend_share", "pgws_share", "pgsa_V2_share", "pgsa_V4_share",
    "kernel_cpu_share_f1f2", "kernel_cpu_share_f1",
    "pgws_minus_exactWB_pp", "pgws_minus_kernel_pp", "pgws_minus_pgsaV2_pp",
    "pgsaV2_minus_exactWB_pp", "pgsaV2_minus_kernel_pp", "exactWB_minus_kernel_pp",
    "--- scheduling bias, pp of WALL on-or-runnable ---",
    "onrq_by_time_pct", "onrq_at_pgsa_pct", "onrq_at_pgws_all_pct",
    "onrq_at_pgws_probe_pct",
    "bias_pgsa_pp", "bias_pgws_all_pp", "bias_pgws_probe_pp",
    "--- the same bias, pp of the CPU SHARE ---",
    "bias_pgsa_pp_share_units", "bias_pgws_all_pp_share_units",
    "bias_pgws_probe_pp_share_units",
    "--- the mechanism: the observers' own scheduling ---",
    "sdelay_mean_us", "sdelay_p50_lo_us", "sdelay_p90_lo_us",
    "cdelay_mean_us", "cdelay_p50_lo_us", "cdelay_p90_lo_us",
    "cburst_mean_us", "cburst_p50_lo_us", "cburst_p90_lo_us",
    "collector_wakeups_hz", "collector_switchins_hz", "collector_probes_hz",
    "pgws_eff_rate_hz", "pgsa_rate_hz",
    "--- overhead ---",
    "collector_oncpu_pct_of_1cpu", "pgsa_sampler_oncpu_pct_of_1cpu",
    "collector_kern_exec_s", "collector_kern_delay_s",
    "--- backend-seconds and misc ---",
    "kern_exec_s", "kern_delay_s", "sched_on_s", "sched_rq_s", "sched_off_s",
    "wait_s", "cmdopen_s", "cpu_out_cmd_s", "straddle_pp",
    "pgws_nonidle_frac_pct", "pgsa_active_frac_pct",
    "torn_pct_of_all_rows", "nonactive_nowait_pct_of_all_rows",
    "pgws_span_misalign_s",
]
keys = list(dict.fromkeys(k for r in runs.values() for k in r))
order += [k for k in keys if k not in order]

cfgnames = sorted(cfgs)
w = 24
print(f"{'quantity':<38s}" + "".join(f"{c:>{w}s}" for c in cfgnames))
print("-" * (38 + w * len(cfgnames)))
for k in order:
    if k.startswith("---"):
        print(k)
        continue
    cells, any_ = [], False
    for c in cfgnames:
        vals = [runs[l][k] for l in cfgs[c] if k in runs[l]]
        if not vals:
            cells.append(f"{'-':>{w}s}")
            continue
        any_ = True
        cells.append(f"{statistics.fmean(vals):>10.2f} [{min(vals):.2f}..{max(vals):.2f}]".rjust(w))
    if any_:
        print(f"{k:<38s}" + "".join(cells))
print()
print("n repeats:", {c: len(cfgs[c]) for c in cfgnames})
