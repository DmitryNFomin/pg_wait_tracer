#!/usr/bin/env python3
"""xtab_summary.py <dir> — mean [min..max] over the repeats of one config.

Reads the per-run xtab_analyze.sh outputs (sat*.txt / unsat*.txt) and prints
one table per regime x mode, so every number carries its spread.
"""
import collections
import glob
import os
import re
import sys

TERMS = [
    ("E_s", r"exact tier numerator E = cpu_in_cmd \+ cpu_out_cmd\s+([\d.-]+)"),
    ("t_wait_rq_s", r"\+ run-queue delay with a wait event still set\s+([\d.-]+)"),
    ("t_wait_on_s", r"\+ on a processor with a wait event still set\s+([\d.-]+)"),
    ("t_idle_rq_s", r"\+ run-queue delay with an IDLE wait event set\s+([\d.-]+)"),
    ("t_idle_on_s", r"\+ on a processor with an IDLE wait event set\s+([\d.-]+)"),
    ("t_gap_s", r"\+ on-or-runnable in a timeline gap \(no interval\)\s+([\d.-]+)"),
    ("t_cpuoff_s", r"- off-CPU \(blocked\) while we call it CPU\s+([\d.-]+)"),
    ("t_unk_s", r"\+- kernel state unknown before first transition\s+([\d.-]+)"),
    ("K_s", r"= kernel numerator K = on \+ rq\s+([\d.-]+)"),
]
PP = [
    ("pp_wait_rq", r"\+ run-queue delay with a wait event still set\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_wait_on", r"\+ on a processor with a wait event still set\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_idle_rq", r"\+ run-queue delay with an IDLE wait event set\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_idle_on", r"\+ on a processor with an IDLE wait event set\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_gap", r"\+ on-or-runnable in a timeline gap \(no interval\)\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_cpuoff", r"- off-CPU \(blocked\) while we call it CPU\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
    ("pp_unk", r"\+- kernel state unknown before first transition\s+[\d.-]+\s+[\d.]+\s+([\d.-]+)"),
]
SCALARS = [
    ("exactWB_pct", r"HEADLINE_AGG \S+ exactWB=([\d.]+)%"),
    ("kernel_f1f2_pct", r"HEADLINE_AGG \S+ .*kernel_f1f2=([\d.]+)%"),
    ("gap_agg_pp", r"HEADLINE_AGG \S+ .*gap=([\d.-]+) pp"),
    ("W_s", r"HEADLINE_AGG \S+ .*W=([\d.]+)s"),
    ("exact_xtab_pct", r"HEADLINE  exact=([\d.]+)%"),
    ("kernel_xtab_pct", r"HEADLINE  exact=[\d.]+%  kernel=([\d.]+)%"),
    ("gap_xtab_pp", r"HEADLINE  .*gap=([\d.-]+) pp"),
    ("win_s", r"^WINDOW \[\d+,\d+\) secs=([\d.]+)"),
    ("tps", r"tps = ([\d.]+)"),
    ("cmds", r"CMD-OPEN total = [\d.]+ s over (\d+) commands"),
    ("closure_resid_s", r"closure residual: K - \(E \+ terms\) = ([\d.eE+-]+) s"),
    ("dump_vs_agg_on_pct", r"on   joined=\s*[\d.]+s  aggregate=\s*[\d.]+s\s+([\d.+-]+)%"),
    ("dump_vs_agg_rq_pct", r"rq   joined=\s*[\d.]+s  aggregate=\s*[\d.]+s\s+([\d.+-]+)%"),
    ("unk_pct_of_wall", r"unk  joined=\s*[\d.]+s  \(([\d.]+)% of wall"),
    ("kern_on_s", r"KERNEL on=([\d.]+)s"),
    ("kern_rq_s", r"KERNEL on=[\d.]+s rq=([\d.]+)s"),
    ("kern_off_s", r"KERNEL on=[\d.]+s rq=[\d.]+s off=([\d.]+)s"),
    ("cpu_out_cmd_s", r"cpu outside cmd = ([\d.]+) s"),
    ("blocked_not_waited_s", r"blocked-but-called-CPU .* = ([\d.]+)s"),
    ("cpuns_wait_kernel_s", r"on-CPU inside WAIT intervals: kernel ([\d.]+)s"),
    ("cpuns_wait_tracer_s", r"on-CPU inside WAIT intervals: kernel [\d.]+s\s+tracer cpu_ns ([\d.]+)s"),
]


def parse(path):
    txt = open(path).read()
    r = {}
    for name, pat in TERMS + PP + SCALARS:
        m = re.search(pat, txt, re.M)
        if m:
            r[name] = float(m.group(1))
    return r


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    runs = {}
    for p in sorted(glob.glob(os.path.join(d, "*.txt"))):
        lbl = os.path.basename(p)[:-4]
        m = re.match(r"^(sat|unsat)(dump|nodump|half|quarter)(\d+)$", lbl)
        if not m:
            continue
        runs.setdefault(m.group(1) + "/" + m.group(2), []).append(parse(p))
    cfgs = sorted(runs)
    keys = [k for k, _ in SCALARS] + [k for k, _ in TERMS] + [k for k, _ in PP]
    seen = [k for k in keys if any(k in r for c in cfgs for r in runs[c])]
    w = max(len(k) for k in seen) + 2
    print(f"{'quantity':<{w}}" + "".join(f"{c:>26}" for c in cfgs))
    print("-" * (w + 26 * len(cfgs)))
    for k in seen:
        row = f"{k:<{w}}"
        for c in cfgs:
            v = [r[k] for r in runs[c] if k in r]
            if not v:
                row += f"{'-':>26}"
            else:
                row += f"{sum(v)/len(v):>12.2f} [{min(v):.2f}..{max(v):.2f}]".rjust(26)
        print(row)
    print("\nrepeats: " + ", ".join(f"{c}={len(runs[c])}" for c in cfgs))


if __name__ == "__main__":
    main()
