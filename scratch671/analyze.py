#!/usr/bin/env python3
"""#671 analysis. PRE-REGISTERED: committed before any run's data was read.

The brief's decision rule names "E ~= G", "S << G", "E > G" without numeric
thresholds, so they are fixed HERE, before data:

    ~=   |X/G - 1| <= 0.10
    <<   X/G <= 0.85
    >    X/G >= 1.10

Branches (verbatim from the brief):
 (1) E~=G and S<<G   -> sampled tier under-reads on 4.18
 (2) S~=G and E>G    -> exact tier over-reads CPU on 4.18
 (3) S~=E~=G in ns but shares still differ -> comparator denominators diverge

Any other pattern is INCONCLUSIVE. G's own bias (it includes between-command
CPU both tiers exclude) is reported as noncmd_cpu_ns_est/G per run and is NOT
used to adjust G -- subtracting a sampled-tier-derived term from the ground
truth would put the thing under test inside the reference.
"""
import json, statistics, sys

NEAR, LOW, HIGH = 0.10, 0.85, 1.10

def cls(ratio):
    if ratio is None: return "?"
    if abs(ratio - 1.0) <= NEAR: return "~=G"
    if ratio <= LOW: return "<<G"
    if ratio >= HIGH: return ">G"
    return "between"

rows = []
for path in sys.argv[1:]:
    for line in open(path):
        line = line.strip()
        if line:
            rows.append(json.loads(line))

hdr = ("run", "pol", "E_ns", "S_ns", "G_ns", "E/G", "S/G", "cpuEx%", "cpuSm%",
       "dpp", "maxD", "res", "steal%", "pol_rb", "noncmd/G")
print("%-18s %-5s %13s %13s %13s %6s %6s %7s %7s %6s %5s %4s %6s %6s %8s" % hdr)
for r in rows:
    print("%-18s %-5s %13.0f %13.0f %13.0f %6.3f %6.3f %7.2f %7.2f %6.2f %5.1f %4s %6.2f %6s %8.4f" % (
        r["label"], r["policy"], r["E_ns"] or 0, r["S_ns"] or 0, r["G_ns"] or 0,
        r["E_over_G"] or 0, r["S_over_G"] or 0,
        r["cpu_share_exact_pct"] or 0, r["cpu_share_sampled_pct"] or 0,
        r.get("cpu_share_delta_pp") or 0, r["max_delta_pp"] or 0,
        r["xval_result"], r["steal_pct"] or 0,
        (r["policy_readback_main"].split("policy=")[1].split()[0] if "policy=" in r["policy_readback_main"] else "?"),
        r["G_bias_ratio_noncmd_over_G"] or 0))

def med(rs, k):
    v = [x[k] for x in rs if x.get(k) is not None]
    return statistics.median(v) if v else None

print()
for host in sorted({r["label"].split("-")[0] for r in rows}):
    for pol in ("other", "fifo"):
        g = [r for r in rows if r["label"].startswith(host + "-" + pol)]
        if not g: continue
        eg, sg = med(g, "E_over_G"), med(g, "S_over_G")
        print("%s %-5s n=%d  median E/G=%.3f (%s)  S/G=%.3f (%s)  "
              "cpu_share_delta=%.2f pp  maxD=%.1f pp  noncmd/G=%.4f" % (
            host, pol, len(g), eg or 0, cls(eg), sg or 0, cls(sg),
            med(g, "cpu_share_delta_pp") or 0, med(g, "max_delta_pp") or 0,
            med(g, "G_bias_ratio_noncmd_over_G") or 0))

el8 = [r for r in rows if r["label"].startswith("el8-other")]
if el8:
    eg, sg = med(el8, "E_over_G"), med(el8, "S_over_G")
    ce, cs_ = cls(eg), cls(sg)
    share_gap = med(el8, "cpu_share_delta_pp") or 0
    if ce == "~=G" and cs_ == "<<G":
        v = "BRANCH (1): sampled tier under-reads CPU on 4.18"
    elif cs_ == "~=G" and ce == ">G":
        v = "BRANCH (2): exact tier over-reads CPU on 4.18"
    elif ce == "~=G" and cs_ == "~=G" and abs(share_gap) >= 5.0:
        v = "BRANCH (3): per-tier DB-time denominators diverge (comparator defect)"
    else:
        v = "INCONCLUSIVE: E/G=%s S/G=%s share_gap=%.2f pp" % (ce, cs_, share_gap)
    print("\nEL8 SCHED_OTHER VERDICT: " + v)
