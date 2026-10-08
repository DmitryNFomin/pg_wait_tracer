#!/usr/bin/env python3
"""Query a finished trace dir via pgwt-server and dump the demo-relevant
numbers: time_model (per-class ms + top sub-events), AAS series range,
transitions node/edge counts, info (num_events), top_events, top_queries.
Usage: measure_query.py TRACE_DIR OUT_DIR"""
import json
import os
import subprocess
import sys


def main():
    trace, out = sys.argv[1], sys.argv[2]
    server = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                          "pgwt-server")
    server = os.path.abspath(server)
    p = subprocess.Popen([server, trace], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, bufsize=1)

    def ask(cmd, **kw):
        req = {"id": ask.n, "cmd": cmd}
        req.update(kw)
        ask.n += 1
        p.stdin.write(json.dumps(req) + "\n")
        p.stdin.flush()
        while True:
            line = p.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except Exception:
                    continue
    ask.n = 1

    res = {}
    for cmd in ("info", "time_model", "aas", "transitions", "top_events",
                "top_queries", "top_sessions", "concurrency"):
        try:
            res[cmd] = ask(cmd)
        except Exception as e:                                # noqa: BLE001
            res[cmd] = {"error": str(e)}
    p.stdin.close()
    p.wait(timeout=30)

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "raw.json"), "w") as f:
        json.dump(res, f, indent=1)

    # ── summary ────────────────────────────────────────────────────────────
    lines = []
    info = res.get("info") or {}
    lines.append(f"num_events       : {info.get('num_events')}")
    lines.append(f"earliest/latest  : {info.get('earliest_wall_ns')} .. "
                 f"{info.get('latest_wall_ns')}")
    span_ns = (info.get("latest_wall_ns") or 0) - (info.get("earliest_wall_ns") or 0)
    span_s = span_ns / 1e9 if span_ns else 0
    lines.append(f"window span      : {span_s:.1f} s")
    if span_s:
        lines.append(f"event rate       : {(info.get('num_events') or 0)/span_s:.0f} events/s")

    tm = res.get("time_model") or {}
    db_time = tm.get("db_time_ms") or 0
    lines.append("")
    lines.append(f"db_time_ms       : {db_time:.1f}")
    for k in ("wall_ms", "cpu_ms", "idle_ms", "aas"):
        if k in tm:
            lines.append(f"{k:17s}: {tm[k]}")
    classes = tm.get("classes") or tm.get("rows") or []
    lines.append("")
    lines.append("CLASS                    ms        % of DB Time")
    rows = []
    for c in classes:
        nm = c.get("name") or c.get("class")
        ms = c.get("ms") or 0
        rows.append((ms, nm, c))
    for ms, nm, c in sorted(rows, reverse=True):
        pct = 100.0 * ms / db_time if db_time else 0
        lines.append(f"{nm:22s} {ms:10.1f}   {pct:6.2f}%")
        for sub in (c.get("events") or c.get("children") or []):
            sms = sub.get("ms") or 0
            spct = 100.0 * sms / db_time if db_time else 0
            lines.append(f"    {sub.get('name'):18s} {sms:10.1f}   {spct:6.2f}%")

    te = res.get("top_events") or {}
    ev = te.get("events") or te.get("rows") or []
    if ev:
        lines.append("")
        lines.append("TOP EVENTS")
        for e in ev[:20]:
            ms = e.get("ms") or e.get("total_ms") or 0
            pct = 100.0 * ms / db_time if db_time else 0
            lines.append(f"  {str(e.get('name')):28s} {ms:10.1f}  {pct:6.2f}%")

    a = res.get("aas") or {}
    series = a.get("series") or a.get("points") or []
    if series:
        tot = []
        for pt in series:
            if isinstance(pt, dict):
                v = pt.get("total") or pt.get("aas")
                if v is None and "classes" in pt:
                    v = sum(pt["classes"].values())
                if v is not None:
                    tot.append(v)
        if tot:
            lines.append("")
            lines.append(f"AAS points       : {len(tot)}")
            lines.append(f"AAS min/median/max: {min(tot):.2f} / "
                         f"{sorted(tot)[len(tot)//2]:.2f} / {max(tot):.2f}")

    tr = res.get("transitions") or {}
    nodes = tr.get("nodes") or []
    edges = tr.get("edges") or []
    lines.append("")
    lines.append(f"transitions      : {len(nodes)} nodes, {len(edges)} edges")
    if nodes:
        lines.append("  nodes: " + ", ".join(
            str(n.get("name") or n.get("id")) for n in nodes))

    tq = res.get("top_queries") or {}
    qs = tq.get("queries") or tq.get("rows") or []
    lines.append("")
    lines.append(f"top_queries rows : {len(qs)}")

    ts = res.get("top_sessions") or {}
    ss = ts.get("sessions") or ts.get("rows") or []
    lines.append(f"top_sessions rows: {len(ss)}")

    text = "\n".join(lines)
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
