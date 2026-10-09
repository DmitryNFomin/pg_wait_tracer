#!/usr/bin/env python3
"""measure_exec_prefix_cost.py — how much does the executions MARKER PREFIX
actually cost, POST-#274?

WHY THIS EXISTS. The paint-latency plan's Phase 3 argues that `executions`
is slow because it reads every structural marker from trace start to the
window's left edge on every request, citing "1398 ms at 98 s of prefix,
4104 ms at 4058 s". Those numbers are PRE-#274: tests/test_data_exec_prefix.py's
own docstring attributes them to the era when the prefix was obtained by
LZ4-DECODING the whole pre-window range ("They used to get those markers by
LZ4-decoding the whole pre-window prefix on EVERY request"). #274 replaced
that with an in-memory retained-marker walk (src/server.c:3028-3100,
xmarks_lower_bound), and no post-#274 prefix timing exists anywhere in
tests/results/. So the number the design decision rests on has never been
measured on the code as it stands.

THE MEASUREMENT. Hold the WINDOW LENGTH constant and vary only how much
prefix sits in front of it:

  near : from = earliest + 5 s     -> prefix ~5 s          (control)
  far  : from = latest - WINDOW    -> prefix ~ capture - W  (the real case)

Both windows are WINDOW seconds long over a uniform synthetic workload, so
the in-window volume is the same and the delta is the prefix and nothing
else. `markers=N` comes from pgwt-server's own PGWT_EXEC_PREFIX_DEBUG line,
so the prefix size is read out of the server rather than assumed.

NOT A GATE. This is a measurement, not a test: it asserts nothing about wall
time and is deliberately absent from tests/unit_tests.list, because a
wall-clock assertion on a shared runner is runner noise dressed up as a gate
(same reasoning as tests/bench_block_agg.c). It prints numbers; a human
decides.

WHAT WOULD MAKE IT LIE, and what is done about it:
  * A prefix of zero on both sides would make any implementation look equal.
    The `near` case is asserted to have a SMALL but non-zero prefix and the
    `far` case a much larger one; if the ratio is not at least 10x the script
    says so and refuses to report a delta.
  * Both runs silently taking the same code path. Each run's `path=` line is
    printed; `far` must read `path=index`, or the number describes the legacy
    decode instead of shipped behaviour.
  * First-request effects: the retained-marker index is built as blocks are
    first decoded, so request 1 pays for construction. Each case therefore
    runs REPS times in ONE server process and reports every repetition, so a
    cold first request cannot be averaged into the steady-state claim.
  * A trace too short for the prefix to matter. The generated capture is
    CAPTURE_S long and the script prints it, so the reader can see the
    prefix:window ratio the number came from.

Usage (from tests/, on Linux with pgwt-server built):
    python3 measure_exec_prefix_cost.py [--capture-s 3600] [--window-s 300]
                                        [--rate 20] [--reps 3] [--json OUT]
"""

import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (          # noqa: E402
    ServerHarness, generate_traces, cleanup_traces,
    CPU, IO_DATA_FILE_READ,
)

S = 1_000_000_000
MS = 1_000_000
BASE = 10_000_000_000_000

MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END = 0xFFFFFFF1
MARKER_PLAN_START = 0xFFFFFFF2
MARKER_PLAN_END = 0xFFFFFFF3
MARKER_CMD_END = 0xFFFFFFF7


def marker(pid, ts, mk, qid=0):
    return {"pid": pid, "ts": ts, "dur": 0, "old": mk, "new": mk, "qid": qid}


def wait(pid, ts, dur, qid, ev=IO_DATA_FILE_READ):
    return {"pid": pid, "ts": ts, "dur": dur, "old": ev, "new": CPU,
            "qid": qid}


def build_uniform(capture_s, rate, n_pids):
    """A uniform statement stream: the same shape everywhere, so the only
    difference between the two windows is how much of it precedes them.

    Each statement is the full lifecycle the prefix pass has to walk --
    PLAN_START, PLAN_END, EXEC_START, two wait events, EXEC_END, CMD_END --
    because a prefix of bare EXEC_STARTs would understate the real marker
    density by a factor of five."""
    events = []
    step_ns = int(S / rate)
    n = capture_s * rate
    for i in range(n):
        pid = 1000 + (i % n_pids)
        t0 = BASE + i * step_ns
        qid = 5000 + (i % 7)
        events.append(marker(pid, t0, MARKER_PLAN_START, qid))
        events.append(marker(pid, t0 + 200_000, MARKER_PLAN_END, qid))
        events.append(marker(pid, t0 + 300_000, MARKER_EXEC_START, qid))
        events.append(wait(pid, t0 + 3 * MS, 2 * MS, qid))
        events.append(wait(pid, t0 + 6 * MS, 1 * MS, qid))
        events.append(marker(pid, t0 + 8 * MS, MARKER_EXEC_END, qid))
        events.append(marker(pid, t0 + 9 * MS, MARKER_CMD_END, qid))
    events.sort(key=lambda e: e["ts"])
    pids = sorted({e["pid"] for e in events})
    return {
        "backends": [{"pid": p, "type": "client", "user": "u", "db": "d"}
                     for p in pids],
        "queries": [{"id": q, "text": "SELECT q%d()" % q}
                    for q in sorted({e["qid"] for e in events if e["qid"]})],
        "events": events,
    }


def read_prefix_lines(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            if line.startswith("pgwt-prefix: "):
                kv = dict(p.split("=", 1)
                          for p in line.split(None, 1)[1].split())
                out.append((kv.get("path"), int(kv.get("markers", -1))))
    return out


def time_case(trace_dir, tmpdir, tag, frm, to, reps):
    """One server process, `reps` identical requests. Every repetition is
    reported: request 1 also pays for building the retained-marker index,
    and averaging that into a steady-state number would overstate the cost
    the design is trying to remove."""
    err = os.path.join(tmpdir, "stderr-%s.txt" % tag)
    times = []
    rows = None
    with ServerHarness(trace_dir, env={"PGWT_EXEC_PREFIX_DEBUG": "1"},
                       stderr_path=err) as srv:
        for _ in range(reps):
            t0 = time.perf_counter()
            resp = srv.query("executions", limit=100,
                             **{"from": str(frm), "to": str(to)})
            times.append((time.perf_counter() - t0) * 1000.0)
            rows = len(resp.get("rows", []) or [])
    return times, rows, read_prefix_lines(err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture-s", type=int, default=3600)
    ap.add_argument("--window-s", type=int, default=300)
    ap.add_argument("--rate", type=int, default=20,
                    help="statements per second")
    ap.add_argument("--pids", type=int, default=8)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    if a.capture_s <= a.window_s + 10:
        print("REFUSE: capture (%ds) must exceed window (%ds) by more than "
              "10s or the 'far' case has no prefix to measure"
              % (a.capture_s, a.window_s))
        return 2

    scen = build_uniform(a.capture_s, a.rate, a.pids)
    n_ev = len(scen["events"])
    print("capture=%ds rate=%d stmt/s pids=%d records=%d window=%ds reps=%d"
          % (a.capture_s, a.rate, a.pids, n_ev, a.window_s, a.reps))

    trace_dir = generate_traces(scen)
    tmpdir = trace_dir
    try:
        with ServerHarness(trace_dir) as srv:
            info = srv.query("info")
        earliest = int(info["from_ns"])
        latest = int(info["to_ns"])
        print("server info: earliest=%d latest=%d span=%.1fs num_events=%s"
              % (earliest, latest, (latest - earliest) / 1e9,
                 info.get("num_events")))
        if latest - earliest < (a.window_s + 20) * S:
            print("REFUSE: retained capture spans only %.1fs, which cannot "
                  "hold a %ds window plus a prefix worth measuring"
                  % ((latest - earliest) / 1e9, a.window_s))
            return 2
        cases = {
            "near": (earliest + 5 * S, earliest + 5 * S + a.window_s * S),
            "far":  (latest - a.window_s * S, latest),
        }
        out = {}
        for tag, (frm, to) in cases.items():
            times, rows, lines = time_case(trace_dir, tmpdir, tag, frm, to,
                                           a.reps)
            out[tag] = {"ms": [round(t, 1) for t in times],
                        "median_ms": round(statistics.median(times), 1),
                        "rows": rows, "prefix_lines": lines}
            print("%-5s from=+%-6ds rows=%-5d ms=%s median=%.1f"
                  % (tag, (frm - BASE) // S, rows,
                     [round(t, 1) for t in times],
                     statistics.median(times)))
            for p, m in lines:
                print("        path=%s markers=%d" % (p, m))

        # Did the fixture actually create the asymmetry being measured?
        near_m = max([m for _, m in out["near"]["prefix_lines"]] or [0])
        far_m = max([m for _, m in out["far"]["prefix_lines"]] or [0])
        print()
        print("prefix markers: near=%d far=%d" % (near_m, far_m))
        if near_m <= 0 or far_m < near_m * 10:
            print("REFUSE to report a delta: the two cases do not differ in "
                  "prefix size by at least 10x (near=%d far=%d), so any "
                  "difference in time is not the prefix." % (near_m, far_m))
            verdict = "inconclusive"
        else:
            far_paths = {p for p, _ in out["far"]["prefix_lines"]}
            if far_paths != {"index"}:
                print("REFUSE: the 'far' case did not use the shipped path "
                      "(path=%s); the number would describe the legacy "
                      "decode." % sorted(far_paths))
                verdict = "wrong-path"
            else:
                delta = out["far"]["median_ms"] - out["near"]["median_ms"]
                print("DELTA (the entire prize) = %.1f ms "
                      "(far median %.1f - near median %.1f)"
                      % (delta, out["far"]["median_ms"],
                         out["near"]["median_ms"]))
                verdict = "measured"
                out["delta_ms"] = round(delta, 1)
        out["verdict"] = verdict
        out["config"] = vars(a)
        out["records"] = n_ev
        if a.json:
            with open(a.json, "w") as fh:
                json.dump(out, fh, indent=2)
            print("wrote %s" % a.json)
    finally:
        cleanup_traces(trace_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
