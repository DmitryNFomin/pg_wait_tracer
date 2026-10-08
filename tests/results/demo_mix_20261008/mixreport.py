import json
import statistics
import sys

d = json.load(open(sys.argv[1]))
tm = d["time_model"]
rows = tm["rows"]
db = tm["db_time_ms"]

print("=== TIME MODEL (900 s nominal window; wall_ms is what the server reports) ===")
for r in rows:
    print("%s%-30s %12.1f ms  %7.2f%%  AAS %.4f"
          % ("  " * r["indent"], r["name"], r["ms"], r["pct"], r["aas"]))
print()
print("db_time_ms   = %.1f" % db)
print("idle_time_ms = %.1f" % tm["idle_time_ms"])
print("wall_ms      = %.1f" % tm["wall_ms"])
print("window AAS   = %.4f" % tm["aas"])
print("fidelity     = %s" % tm.get("fidelity"))

print()
print("=== AAS SERIES ===")
a = d["aas"]
b = a.get("buckets") or []
keys = ("cpu", "io", "lock", "lwlock", "ipc", "client", "timeout",
        "bufferpin", "activity", "extension", "unknown", "offcpu")
tot = [sum(x.get(k, 0) or 0 for k in keys) for x in b]
print("buckets      = %d, bucket_ns = %s" % (len(b), a.get("bucket_ns")))
print("max_aas (server) = %s" % a.get("max_aas"))
if tot:
    print("total AAS per bucket: min %.3f  p50 %.3f  p90 %.3f  max %.3f"
          % (min(tot), statistics.median(tot),
             sorted(tot)[int(0.9 * (len(tot) - 1))], max(tot)))
    # per-class peaks, so "which class ever dominates" is visible
    for k in keys:
        v = [x.get(k, 0) or 0 for x in b]
        if max(v) > 0.001:
            print("   %-10s peak %.3f  median %.3f" % (k, max(v), statistics.median(v)))

print()
print("=== INFO ===")
i = d["info"]
for k in sorted(i):
    if not isinstance(i[k], (list, dict)):
        print("  %-24s %s" % (k, i[k]))

print()
print("=== TRANSITIONS ===")
t = d["transitions"]
for k, v in t.items():
    print("  %-14s %s" % (k, len(v) if isinstance(v, list) else v))

print()
print("=== TOP QUERIES / SESSIONS ===")
for cmd, keyopts in (("top_queries", ("queries", "rows")),
                     ("top_sessions", ("sessions", "rows"))):
    o = d.get(cmd) or {}
    for k in keyopts:
        if isinstance(o.get(k), list):
            print("  %-14s %d rows" % (cmd, len(o[k])))
            break
    else:
        print("  %-14s keys=%s" % (cmd, list(o.keys())))
