#!/usr/bin/env python3
"""#671: second pass over the retained per-run dirs (run locally, after rsync).

Adds what summary.txt does not carry: RAW_GATE_REJECTED_EXACT_CPU_NS, and an
Extension-row-removed recomputation of the two CPU shares -- the EL8 VM's
PostgreSQL preloads pg_wait_sampling and gate-3's does not, so EL8 carries an
extra background worker parked in Extension for the whole window. That is a
HOST CONFIGURATION confound in the DB-time denominator and is disclosed, not
silently absorbed.
"""
import json, os, re, sys

root = sys.argv[1]
rows = []
for d in sorted(os.listdir(root)):
    p = os.path.join(root, d)
    sp = os.path.join(p, "summary.txt")
    xp = os.path.join(p, "xval.txt")
    if not (os.path.isfile(sp) and os.path.isfile(xp)):
        continue
    m = [l for l in open(sp) if l.startswith("JSONL ")]
    if not m:
        print("REFUSED %s: no JSONL line in summary.txt" % d); continue
    r = json.loads(m[0][6:])
    x = open(xp).read()
    def kv(k):
        mm = re.search(r'^%s\s+([-\d.]+)' % k, x, re.M)
        return float(mm.group(1)) if mm else None
    r["gate_rejected_exact_cpu_ns"] = kv("RAW_GATE_REJECTED_EXACT_CPU_NS")
    r["gate_rejected_intervals"] = kv("RAW_GATE_REJECTED_INTERVALS")
    r["noncpu_exact_ns"] = kv("RAW_NONCPU_EXACT_NS")
    r["noncpu_sampled_ns"] = kv("RAW_NONCPU_SAMPLED_NS")
    # Extension row (pg_wait_sampling collector) -- shares, then ns
    em = re.search(r'^Extension\s+([\d.]+)%\s+([\d.]+)%', x, re.M)
    ex_pct, sm_pct = (float(em.group(1)), float(em.group(2))) if em else (0.0, 0.0)
    r["extension_exact_pct"] = ex_pct
    r["extension_sampled_pct"] = sm_pct
    TE, TS, E, S = r["total_exact_ns"], r["total_sampled_ns"], r["E_ns"], r["S_ns"]
    te2 = TE - TE * ex_pct / 100.0
    ts2 = TS - TS * sm_pct / 100.0
    r["cpu_share_exact_noext_pct"] = round(100.0 * E / te2, 2) if te2 else None
    r["cpu_share_sampled_noext_pct"] = round(100.0 * S / ts2, 2) if ts2 else None
    r["cpu_share_delta_noext_pp"] = round(r["cpu_share_exact_noext_pct"] - r["cpu_share_sampled_noext_pct"], 2)
    # E with the command-gate-rejected CPU added back: the exact tier's
    # measured on-CPU before the gate drops what it cannot attribute.
    gr = r["gate_rejected_exact_cpu_ns"] or 0.0
    r["E_plus_gaterej_ns"] = E + gr
    r["E_plus_gaterej_over_G"] = round((E + gr) / r["G_ns"], 4) if r["G_ns"] else None
    rows.append(r)

out = os.path.join(root, "all_enriched.jsonl")
with open(out, "w") as f:
    for r in rows:
        f.write(json.dumps(r, sort_keys=True) + "\n")
print("wrote %s (%d runs)" % (out, len(rows)))
