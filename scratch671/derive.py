#!/usr/bin/env python3
"""INSTRUMENTATION for #671: derive E / S / G and covariates for one run.

G = sum over client-backend pids present at BOTH window edges of
    delta(/proc/<pid>/schedstat field 1) -- the kernel's own on-CPU ns.
"""
import json, os, re, sys

out, t0, t1, label, policy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]

def read(p):
    try:
        return open(os.path.join(out, p)).read()
    except OSError:
        return ""

def kv(txt, key, cast=float):
    m = re.search(r'^%s\s+([-\d.]+)' % re.escape(key), txt, re.M)
    return cast(m.group(1)) if m else None

x = read("xval.txt")
E  = kv(x, "RAW_CPU_EXACT_NS")
S  = kv(x, "RAW_CPU_SAMPLED_NS")
TE = kv(x, "RAW_TOTAL_EXACT_NS")
TS = kv(x, "RAW_TOTAL_SAMPLED_NS")
WIN = kv(x, "RAW_WINDOW_S")
PER = kv(x, "RAW_SAMPLE_PERIOD_NS")
m = re.search(r'^Samples in window:\s+(\d+)', x, re.M); NSAMP = int(m.group(1)) if m else None
m = re.search(r'Max share disagreement.*?:\s+([\d.]+) pp \((\S+)\)', x); MAXD, MAXEV = (float(m.group(1)), m.group(2)) if m else (None, None)
m = re.search(r'^RESULT: (\w+)', x, re.M); RESULT = m.group(1) if m else "ABSENT"

def snap(p):
    d = {}
    for line in read(p).splitlines():
        a = line.split()
        if len(a) == 2:
            d[a[0]] = int(a[1])
    return d
s0, s1 = snap("schedstat_t0.txt"), snap("schedstat_t1.txt")
both = sorted(set(s0) & set(s1))
G = sum(s1[p] - s0[p] for p in both)
G_dropped = sorted(set(s0) ^ set(s1))

def cpuline(p):
    a = read(p).split()
    return [int(v) for v in a[1:]] if a else []
b, a_ = cpuline("procstat_before.txt"), cpuline("procstat_after.txt")
steal_pct = None; cpu_busy_pct = None
if b and a_ and len(b) >= 8:
    d = [a_[i] - b[i] for i in range(min(len(a_), len(b)))]
    tot = sum(d)
    if tot > 0:
        steal_pct = 100.0 * d[7] / tot
        cpu_busy_pct = 100.0 * (tot - d[3] - d[4]) / tot

def mj(p):
    t = read(p)
    i = t.find("{")
    try:
        return json.loads(t[i:]) if i >= 0 else {}
    except Exception:
        return {}
mb, ma = mj("metrics_before.json"), mj("metrics_after.json")
def dm(k):
    if k in mb and k in ma:
        try:
            return ma[k] - mb[k]
        except TypeError:
            return None
    return None
noncmd = dm("noncmd_cpu_samples_total")
samples = dm("samples_total")
tickfail = dm("sampled_attr_tick_read_failures_total")
missed = dm("sampler_ticks_missed_total")
per_ns = PER if PER else 1e8
noncmd_ns = noncmd * per_ns if noncmd is not None else None

wall_s = (t1 - t0) / 1e9
r = {}
r["label"] = label; r["policy"] = policy
r["E_ns"] = E; r["S_ns"] = S; r["G_ns"] = G
r["G_pids"] = len(both); r["G_pid_churn"] = len(G_dropped)
r["total_exact_ns"] = TE; r["total_sampled_ns"] = TS
r["win_s_comparator"] = WIN; r["win_s_wallclock"] = round(wall_s, 3)
r["samples_in_window"] = NSAMP; r["sample_period_ns"] = PER
r["max_delta_pp"] = MAXD; r["max_delta_event"] = MAXEV; r["xval_result"] = RESULT
r["cpu_share_exact_pct"] = round(100.0 * E / TE, 2) if E is not None and TE else None
r["cpu_share_sampled_pct"] = round(100.0 * S / TS, 2) if S is not None and TS else None
if r["cpu_share_exact_pct"] is not None and r["cpu_share_sampled_pct"] is not None:
    r["cpu_share_delta_pp"] = round(r["cpu_share_exact_pct"] - r["cpu_share_sampled_pct"], 2)
r["E_over_G"] = round(E / G, 4) if E and G else None
r["S_over_G"] = round(S / G, 4) if S and G else None
r["noncmd_cpu_samples"] = noncmd
r["noncmd_cpu_ns_est"] = noncmd_ns
r["G_bias_ratio_noncmd_over_G"] = round(noncmd_ns / G, 4) if noncmd_ns is not None and G else None
r["samples_total_delta"] = samples
r["tick_read_failures_delta"] = tickfail
r["sampler_ticks_missed_delta"] = missed
r["steal_pct"] = round(steal_pct, 3) if steal_pct is not None else None
r["cpu_busy_pct"] = round(cpu_busy_pct, 2) if cpu_busy_pct is not None else None
r["loadavg_before"] = read("loadavg_before.txt").strip()
r["loadavg_after"] = read("loadavg_after.txt").strip()
r["uname_r"] = read("uname-r.txt").strip()
r["vendor"] = next((l.split(":")[1].strip() for l in read("lscpu.txt").splitlines() if l.startswith("Vendor ID")), "")
r["model"] = next((l.split(":")[1].strip() for l in read("lscpu.txt").splitlines() if l.startswith("Model name")), "")
r["commit"] = read("commit.txt").strip()
r["policy_readback_main"] = next((l for l in read("policy_readback.txt").splitlines() if l), "")

# Refusals: a missing field must read as ABSENT, never as a measured zero.
missing = [k for k in ("E_ns", "S_ns", "total_exact_ns", "total_sampled_ns", "max_delta_pp") if r[k] is None]
if missing:
    r["REFUSED"] = "comparator fields absent: " + ",".join(missing)
if G <= 0:
    r["REFUSED"] = r.get("REFUSED", "") + " ; G<=0 (no client-backend on-CPU time observed)"

print(json.dumps(r, indent=1, sort_keys=True))
print("JSONL " + json.dumps(r, sort_keys=True))
