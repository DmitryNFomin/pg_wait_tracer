#!/usr/bin/env python3
"""test_protocol_drift.py — Catch mock-vs-real protocol drift (Phase B1).

The web UI is tested against tests/mock_server.py, which mirrors the JSON-line
protocol of the real pgwt-server (src/server.c).  If the real server's response
shape changes and the mock is not updated (or vice versa), the UI tests keep
passing against a protocol that no longer exists.  This test:

  1. Generates a small synthetic trace fixture via tests/gen_test_traces
     (nothing committed — built fresh on every run, also in CI).
  2. Sends the same set of protocol commands to the real pgwt-server
     (stdin/stdout JSON lines) and to mock_server.handle_request().
  3. Compares the response *schemas* — key sets and value types, recursively —
     not exact values (the fixture data and the canned mock data differ).

Run: python3 tests/test_protocol_drift.py
  (No root, no PG, no SSH, no third-party deps — needs pgwt-server and
   tests/gen_test_traces built.)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LWLOCK_WAL_WRITE, TIMEOUT_PG_SLEEP,
)
import mock_server

# Event IDs for the fixture (PG18 numbering, class_byte << 24 | event_num)
LOCK_TXN = 0x03000008    # Lock:transactionid
IO_WRITE = 0x0A000018    # IO:DataFileWrite

# Lifecycle markers (src/pg_wait_tracer.h) — drive variants + exec/plan stats
MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END   = 0xFFFFFFF1
MARKER_PLAN_START = 0xFFFFFFF2
MARKER_PLAN_END   = 0xFFFFFFF3

BASE = 10_000_000_000_000  # 10000s in ns (> 1hr, see server_harness)
MS = 1_000_000

# ── Fixture scenario ─────────────────────────────────────────────────────────
# Built to make every view non-empty:
#   - several wait classes (time_model / top_events / aas / heatmap)
#   - multiple backends + queries (top_sessions / top_queries / fingerprints)
#   - CPU↔wait alternation (transitions)
#   - blocker on CPU while another pid waits on a Lock (lock_chains)
#   - two pids waiting on the same event at the same time (interference,
#     concurrency bursts)
#   - plan/exec lifecycle markers around pid 1000's events (variants,
#     exec/plan stats on top_queries rows) — two executions of the same
#     query so percentile fields (p95/p99) are emitted too


def _marker(pid, qid, ts, marker):
    return {"pid": pid, "ts": ts, "dur": 0, "old": marker, "new": 0, "qid": qid}

def _seq(pid, qid, start, pairs):
    """Expand [(wait_event, dur_ms), ...] into alternating wait/CPU events."""
    events = []
    ts = start
    for ev, dur_ms in pairs:
        dur = dur_ms * MS
        ts += dur
        events.append({"pid": pid, "ts": ts, "dur": dur,
                       "old": ev, "new": CPU if ev != CPU else IO_DATA_FILE_READ,
                       "qid": qid})
    return events


SCENARIO = {
    "backends": [
        {"pid": 1000, "type": "client", "user": "u", "db": "d"},
        {"pid": 1001, "type": "client", "user": "u", "db": "d"},
        {"pid": 1002, "type": "client", "user": "u", "db": "d"},
        {"pid": 1003, "type": "client", "user": "u", "db": "d"},
        {"pid": 1004, "type": "client", "user": "u", "db": "d"},
        {"pid": 1005, "type": "client", "user": "u", "db": "d"},
        {"pid": 1006, "type": "parallel_worker", "leader_pid": 1000},
    ],
    "queries": [
        {"id": 100, "text": "SELECT 1"},
        {"id": 200, "text": "UPDATE t SET x = x + 1"},
    ],
    "events": (
        # PID 1000: blocker — on CPU the whole time, alternating with IO
        _seq(1000, 100, BASE, [(CPU, 10), (IO_DATA_FILE_READ, 2),
                               (CPU, 10), (IO_WRITE, 3), (CPU, 5)])
        # PID 1001: waits on Lock:transactionid while 1000 is on CPU
        + [{"pid": 1001, "ts": BASE + 8 * MS, "dur": 7 * MS,
            "old": LOCK_TXN, "new": CPU, "qid": 200},
           {"pid": 1001, "ts": BASE + 10 * MS, "dur": 2 * MS,
            "old": CPU, "new": LWLOCK_WAL_WRITE, "qid": 200},
           {"pid": 1001, "ts": BASE + 14 * MS, "dur": 4 * MS,
            "old": LWLOCK_WAL_WRITE, "new": CPU, "qid": 200}]
        # PIDs 1002/1003: overlapping IO:DataFileRead waits (interference)
        + [{"pid": 1002, "ts": BASE + 5 * MS, "dur": 5 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
           {"pid": 1002, "ts": BASE + 7 * MS, "dur": 2 * MS,
            "old": CPU, "new": TIMEOUT_PG_SLEEP, "qid": 100},
           {"pid": 1002, "ts": BASE + 9 * MS, "dur": 2 * MS,
            "old": TIMEOUT_PG_SLEEP, "new": CPU, "qid": 100},
           {"pid": 1003, "ts": BASE + 6 * MS, "dur": 4 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 200},
           {"pid": 1003, "ts": BASE + 8 * MS, "dur": 2 * MS,
            "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200}]
        # PIDs 1004/1005: also waiting on IO:DataFileRead in the same window
        # so 4+ sessions overlap on one event → a concurrency burst
        + [{"pid": 1004, "ts": BASE + 6 * MS, "dur": 5 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
           {"pid": 1004, "ts": BASE + 8 * MS, "dur": 2 * MS,
            "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100},
           {"pid": 1005, "ts": BASE + 7 * MS, "dur": 6 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 200},
           {"pid": 1005, "ts": BASE + 9 * MS, "dur": 2 * MS,
            "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200}]
        # PID 1006 is a parallel worker for leader 1000 (B6 leader_pid lane).
        + [{"pid": 1006, "ts": BASE + 11 * MS, "dur": 3 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100}]
        # PID 1000 lifecycle: plan+exec span around the wait sequence above,
        # then a second short plan+exec of the same query (different wait
        # pattern → second variant; 2 samples → p95/p99 lifecycle fields)
        + [_marker(1000, 100, BASE - 2 * MS, MARKER_PLAN_START),
           _marker(1000, 100, BASE - 1 * MS, MARKER_PLAN_END),
           _marker(1000, 100, BASE, MARKER_EXEC_START),
           _marker(1000, 100, BASE + 30 * MS + 1000, MARKER_EXEC_END),
           _marker(1000, 100, BASE + 31 * MS, MARKER_PLAN_START),
           _marker(1000, 100, BASE + 31 * MS + 500_000, MARKER_PLAN_END),
           _marker(1000, 100, BASE + 32 * MS, MARKER_EXEC_START),
           {"pid": 1000, "ts": BASE + 34 * MS, "dur": 1 * MS,
            "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
           _marker(1000, 100, BASE + 35 * MS, MARKER_EXEC_END)]
    ),
}
# Keep the event stream globally time-ordered (the real pipeline writes
# events in capture order)
SCENARIO["events"].sort(key=lambda e: e["ts"])

# ── Commands to compare ──────────────────────────────────────────────────────
# Each entry: (label, request-kwargs for ServerHarness.query()).
# This is the protocol surface the web UI exercises through the mock.

COMMANDS = [
    ("info",                  "info",             {}),
    ("time_model",            "time_model",       {}),
    ("aas",                   "aas",              {"buckets": 30}),
    ("aas detail=events",     "aas",              {"buckets": 30, "detail": "events"}),
    ("top_events",            "top_events",       {}),
    ("top_events class=io",   "top_events",       {"filters": {"class": "io"}}),
    ("top_sessions",          "top_sessions",     {}),
    ("top_queries",           "top_queries",      {}),
    ("heatmap",               "heatmap",          {}),
    ("session_timeline pid",  "session_timeline", {"filters": {"pid": 1000}}),
    ("executions",            "executions",       {"limit": 100}),
    ("execution_detail",      "execution_detail", {"pid": 1000,
                                                     "start_ns": str(BASE),
                                                     "end_ns": str(BASE + 30 * MS + 1000)}),
    ("exec_scatter",          "exec_scatter",     {"max_points": 2000}),
    ("transitions",           "transitions",      {}),
    ("fingerprints",          "fingerprints",     {}),
    ("concurrency",           "concurrency",      {"buckets": 30}),
    ("lock_chains",           "lock_chains",      {}),
    ("interference",          "interference",     {}),
    ("variants",              "variants",         {"buckets": 20}),
]

# Schema paths where mock and real server are *known* to differ and the
# difference is accepted for now.  Every entry needs a justification.
# Keep this list empty if at all possible — entries here are documented debt.
ALLOWED_DIFFS = {
    # (none)
}

# Paths whose dict keys are data values, not protocol fields (e.g. class_pct
# maps wait-class name → pct and only includes classes with pct >= 0.1, see
# src/server.c).  Key sets are compared loosely: only value types must match.
MAP_PATHS = {
    "rows[].class_pct",
}


# ── Schema extraction and comparison ─────────────────────────────────────────

def schema_of(value):
    """Recursively reduce a JSON value to its shape (keys + type classes)."""
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if value is None:
        return "null"
    if isinstance(value, list):
        return ["list", merge_schemas([schema_of(v) for v in value])]
    if isinstance(value, dict):
        return {k: schema_of(v) for k, v in value.items()}
    return f"unknown:{type(value).__name__}"


def merge_schemas(schemas):
    """Merge schemas of list elements: union of dict keys, common types."""
    if not schemas:
        return "empty"
    if all(isinstance(s, dict) for s in schemas):
        merged = {}
        for s in schemas:
            for k, v in s.items():
                if k in merged and merged[k] != v:
                    merged[k] = merge_schemas([merged[k], v])
                else:
                    merged[k] = v
        return merged
    if all(isinstance(s, list) and s and s[0] == "list" for s in schemas):
        return ["list", merge_schemas([s[1] for s in schemas])]
    uniq = []
    for s in schemas:
        if s not in uniq:
            uniq.append(s)
    if len(uniq) == 1:
        return uniq[0]
    # "null" merges into any other single type (optional field)
    non_null = [s for s in uniq if s != "null"]
    if len(non_null) == 1:
        return non_null[0]
    return f"mixed({','.join(str(u) for u in uniq)})"


def diff_schemas(real, mock, path, diffs):
    """Collect human-readable differences between two schemas."""
    if isinstance(real, dict) and isinstance(mock, dict):
        if path in MAP_PATHS:
            # Data-keyed map: compare merged value schemas, not key sets.
            diff_schemas(merge_schemas(list(real.values())),
                         merge_schemas(list(mock.values())),
                         f"{path}.<value>", diffs)
            return
        for k in sorted(set(real) | set(mock)):
            sub = f"{path}.{k}" if path else k
            if k not in real:
                diffs.append(f"{sub}: only in mock (type {mock[k]!r})")
            elif k not in mock:
                diffs.append(f"{sub}: only in real server (type {real[k]!r})")
            else:
                diff_schemas(real[k], mock[k], sub, diffs)
        return
    if isinstance(real, list) and isinstance(mock, list) \
            and real and mock and real[0] == "list" and mock[0] == "list":
        if real[1] == "empty" or mock[1] == "empty":
            # One side has no elements to infer a shape from — cannot compare
            # deeper.  The fixture/canned data should be fixed to be non-empty;
            # report it so it does not pass silently.
            if real[1] == "empty" and mock[1] != "empty":
                diffs.append(f"{path}[]: real server returned an empty list "
                             f"(fixture produces no data?) — cannot check shape")
            elif mock[1] == "empty" and real[1] != "empty":
                diffs.append(f"{path}[]: mock returned an empty list "
                             f"(canned data missing?) — cannot check shape")
            return
        diff_schemas(real[1], mock[1], f"{path}[]", diffs)
        return
    if real != mock:
        diffs.append(f"{path}: real={real!r} mock={mock!r}")


def filter_allowed(diffs):
    kept = []
    for d in diffs:
        prefix = d.split(":")[0]
        if prefix in ALLOWED_DIFFS:
            print(f"  ALLOWED: {d} ({ALLOWED_DIFFS[prefix]})")
            continue
        kept.append(d)
    return kept


# ── Main ─────────────────────────────────────────────────────────────────────

def mock_query(cmd, req_id, **kwargs):
    """Send the same logical request to the mock's dispatch function."""
    msg = {"id": req_id, "cmd": cmd}
    if "buckets" in kwargs:
        msg["buckets"] = kwargs["buckets"]
        msg["num_buckets"] = kwargs["buckets"]  # mock reads num_buckets
    if "detail" in kwargs:
        msg["detail"] = kwargs["detail"]
    if "filters" in kwargs:
        msg["filters"] = kwargs["filters"]
    for key in ("limit", "max_points", "pid", "query_id",
                "start_ns", "end_ns"):
        if key in kwargs:
            msg[key] = kwargs[key]
    return mock_server.handle_request(msg)


def main():
    t = TestRunner("test_protocol_drift")
    trace_dir = generate_traces(SCENARIO)
    try:
        with ServerHarness(trace_dir) as srv:
            for label, cmd, kwargs in COMMANDS:
                real = srv.query(cmd, **kwargs)
                mock = mock_query(cmd, req_id=real.get("id", 0), **kwargs)

                t.check("error" not in real,
                        f"[{label}] real server answered without error"
                        + (f" (got: {real.get('error')})" if "error" in real else ""))
                t.check("error" not in mock,
                        f"[{label}] mock answered without error"
                        + (f" (got: {mock.get('error')})" if "error" in mock else ""))
                if "error" in real or "error" in mock:
                    continue

                diffs = []
                diff_schemas(schema_of(real), schema_of(mock), "", diffs)
                diffs = filter_allowed(diffs)
                t.check(not diffs, f"[{label}] response schemas match")
                for d in diffs:
                    print(f"    DRIFT: {d}")
    finally:
        cleanup_traces(trace_dir)

    ok = t.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
