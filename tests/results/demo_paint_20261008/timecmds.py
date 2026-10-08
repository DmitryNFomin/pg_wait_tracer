#!/usr/bin/env python3
"""Time pgwt-server commands at the STDIN/STDOUT boundary -- no transport, no
browser, no instrumentation needed. Server-side wall time per command.

Usage: timecmds.py SERVER TRACE_DIR [WINDOW_S]
"""
import json
import subprocess
import sys
import time

server, trace = sys.argv[1], sys.argv[2]
window_s = float(sys.argv[3]) if len(sys.argv) > 3 else 900.0

p = subprocess.Popen([server, trace], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True, bufsize=1)
n = [0]


def ask(cmd, **kw):
    n[0] += 1
    req = {"id": n[0], "cmd": cmd}
    req.update(kw)
    t0 = time.monotonic()
    p.stdin.write(json.dumps(req) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            return None, (time.monotonic() - t0) * 1000.0
        line = line.strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
            except Exception:
                continue
            return d, (time.monotonic() - t0) * 1000.0


info, _ = ask("info")
latest = info.get("latest_wall_ns") or info["to_ns"]
earliest = info.get("earliest_wall_ns") or info["from_ns"]
frm = max(earliest, latest - int(window_s * 1e9))
span = (latest - frm) / 1e9
print("trace      : %s" % trace)
print("num_events : %d" % info["num_events"])
print("window     : %.1f s  (from_ns=%d to_ns=%d)" % (span, frm, latest))
print()
print("%-18s %10s  %s" % ("command", "ms", "notes"))
print("-" * 72)

CMDS = [
    ("variants", {}),
    ("transitions", {}),
    ("top_queries", {}),
    ("concurrency", {}),
    ("executions", {}),
    ("exec_scatter", {}),
    ("time_model", {}),
    ("aas", {}),
    ("top_events", {}),
    ("top_sessions", {}),
    ("heatmap", {}),
]
rows = []
for cmd, extra in CMDS:
    d, ms = ask(cmd, from_ns=frm, to_ns=latest, **extra)
    note = ""
    if d is None:
        note = "NO RESPONSE"
    elif "error" in d:
        note = "error: %s" % str(d["error"])[:40]
    else:
        fid = d.get("fidelity")
        if fid:
            note = "fidelity=%s" % fid
        for k in ("nodes", "links", "rows", "buckets", "queries", "points",
                  "sessions", "events", "executions"):
            v = d.get(k)
            if isinstance(v, list):
                note += " %s=%d" % (k, len(v))
    rows.append((cmd, ms, note))
    print("%-18s %10.1f  %s" % (cmd, ms, note))

p.stdin.close()
p.wait(timeout=30)

print()
rows.sort(key=lambda r: -r[1])
print("slowest: %s at %.0f ms" % (rows[0][0], rows[0][1]))
print("sum of all commands: %.0f ms" % sum(r[1] for r in rows))
