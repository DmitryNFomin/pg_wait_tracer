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

# #487 requires idle on every transitions node; #515 requires the response's
# idle_classification_complete readiness field. Pin the minimum independently
# of the mock so lowering all production constants still fails the gate.
REQUIRED_PROTOCOL_REV = 2

# Event IDs for the fixture (PG18 numbering, class_byte << 24 | event_num)
LOCK_TXN = 0x03000008    # Lock:transactionid
IO_WRITE = 0x0A000018    # IO:DataFileWrite
LOCK_RELATION = 0x03000000               # Lock:relation
TIMEOUT_CHECKPOINT_WRITE_DELAY = 0x09000001  # Timeout:CheckpointWriteDelay (pacing)

# Lifecycle markers (src/pg_wait_tracer.h) — drive variants + exec/plan stats
MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END   = 0xFFFFFFF1
MARKER_PLAN_START = 0xFFFFFFF2
MARKER_PLAN_END   = 0xFFFFFFF3
# #455: the command boundaries the backend-activity span is measured from.
# Without them pid 1000's span is UNMEASURED (both boundaries null, no
# completed duration), which is a real and separately-tested state but not
# the one the mock's canned span describes -- and a fixture that only ever
# produces the degenerate state would let the measured shape drift unnoticed.
MARKER_CMD_START  = 0xFFFFFFF6
MARKER_CMD_END    = 0xFFFFFFF7

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
        {"pid": 1010, "type": "client", "user": "u", "db": "d"},
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
        # PID 1010: #487 fixture — one pacing Timeout node (idle==true) and one
        # Lock node (idle==false), so the transitions `idle` field can be
        # checked against two cases the client cannot derive on its own.
        + _seq(1010, 100, BASE, [(TIMEOUT_CHECKPOINT_WRITE_DELAY, 3),
                                 (LOCK_RELATION, 3)])
        # PID 1000 lifecycle: plan+exec span around the wait sequence above,
        # then a second short plan+exec of the same query (different wait
        # pattern → second variant; 2 samples → p95/p99 lifecycle fields)
        # #455: ONE command window around BOTH executions, so the real
        # server's containing span is CLOSED, fully MEASURED and
        # multi-execution -- the shape mock_server.py's _mock_span() serves.
        + [_marker(1000, 100, BASE - 3 * MS, MARKER_CMD_START),
           _marker(1000, 100, BASE + 36 * MS, MARKER_CMD_END),
           _marker(1000, 100, BASE - 2 * MS, MARKER_PLAN_START),
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


def check_protocol_revision(t, srv):
    """The real server's reported protocol is the C side at runtime (the
    binary), which the Go source-level agreement test cannot see. Required
    transitions fields (#487, #515) need both agreement and a pinned minimum.
    """
    real = srv.query("info")
    mock = mock_query("info", req_id=real.get("id", 0))
    for source, response in (("real", real), ("mock", mock)):
        t.check("error" not in response,
                f"[protocol revision] {source} answered without error"
                + (f" (got: {response.get('error')})"
                   if "error" in response else ""))
        t.check("protocol" in response,
                f"[protocol revision] {source} info carries protocol")
        protocol = response.get("protocol")
        valid = isinstance(protocol, int) and not isinstance(protocol, bool)
        t.check(valid,
                f"[protocol revision] {source} protocol is an integer "
                f"(got {protocol!r})")
        if source == "real" and valid:
            t.check(protocol == mock_server.PROTOCOL_REV,
                    f"[protocol revision] real protocol {protocol} == mock "
                    f"PROTOCOL_REV {mock_server.PROTOCOL_REV}")
            t.check(protocol >= REQUIRED_PROTOCOL_REV,
                    f"[protocol revision] real protocol {protocol} >= "
                    f"required protocol {REQUIRED_PROTOCOL_REV}")


def check_transitions_idle_flag(t, srv):
    """#487: every transitions node must carry an authoritative `idle` boolean,
    because the Timeout half of pgwt_is_idle_event() depends on a runtime-only
    pacing mask the client cannot replicate. Named per the two cases that
    exercise both branches: a pacing Timeout (idle) and a Lock (never idle).

    #501: the per-node loop below checks every node in both the real server's
    and mock_server's responses for TYPE only -- `isinstance(idle, bool)` --
    so a partial catalog (a node present without the flag, or with a
    non-boolean value) cannot slip through unnoticed. It does NOT check that
    an unnamed node's idle VALUE is correct; only the two named cases above
    are value-pinned. Recomputing the correct value for every node here would
    mean duplicating pgwt_is_idle_event()'s classification logic in the test,
    which would only prove two copies of the same rule agree. The "present
    but wrong" case -- e.g. an incomplete pacing mask making a Timeout node's
    idle flag present and boolean but wrong -- is tracked separately as #515,
    not covered by this function."""
    real = srv.query("transitions")
    t.check("error" not in real,
            "[transitions idle] real server answered without error"
            + (f" (got: {real.get('error')})" if "error" in real else ""))
    mock = mock_query("transitions", req_id=real.get("id", 0))
    t.check("error" not in mock,
            "[transitions idle] mock answered without error"
            + (f" (got: {mock.get('error')})" if "error" in mock else ""))
    for source, response in (("real", real), ("mock", mock)):
        if "error" in response:
            continue
        nodes = response.get("nodes", [])
        t.check(len(nodes) > 0,
                f"[transitions idle] {source} nodes array is non-empty")
        for node in nodes:
            idle = node.get("idle")
            t.check(isinstance(idle, bool),
                    f"[transitions idle] {source} node "
                    f"{node.get('name', '<unnamed>')!r} idle is a boolean "
                    f"(got {idle!r})")
    if "error" in real:
        return
    by_name = {n.get("name", "<unnamed>"): n for n in real.get("nodes", [])}
    cases = [
        ("Timeout:CheckpointWriteDelay", True),
        ("Lock:relation", False),
    ]
    for name, expected in cases:
        t.check(name in by_name,
                f"[transitions idle] node {name!r} present in response")
        if name not in by_name:
            continue
        node = by_name[name]
        t.check("idle" in node,
                f"[transitions idle] {name!r} node carries an idle field")
        t.check(node.get("idle") is expected,
                f"[transitions idle] {name!r} idle == {expected} "
                f"(got {node.get('idle')!r})")


def check_top_sessions_cpu_fields(t, srv):
    """#562: the measured-CPU contract on Top Sessions, asserted POSITIVELY.

    The schema comparison above is symmetric: if BOTH the real server and the
    mock omitted these fields it would pass, which is the false negative that
    matters here -- the fields went in to say which quantity the cpu_pct
    column is, and a response without them is the ambiguity #562 is about.
    So the real server's own response is checked for presence and type, and
    every row for its remainder, independently of the mock.
    """
    real = srv.query("top_sessions")
    t.check("error" not in real,
            "[sessions cpu] real server answered without error"
            + (f" (got: {real.get('error')})" if "error" in real else ""))
    if "error" in real:
        return
    t.check(isinstance(real.get("has_measured_cpu"), bool),
            "[sessions cpu] has_measured_cpu is a boolean "
            f"(got {real.get('has_measured_cpu')!r})")
    for field in ("offcpu_ms", "cpu_clamped_ms"):
        v = real.get(field)
        t.check(isinstance(v, (int, float)) and not isinstance(v, bool),
                f"[sessions cpu] {field} is a number (got {v!r})")
    rows = real.get("rows", [])
    t.check(len(rows) > 0, "[sessions cpu] the fixture produces session rows")
    for row in rows:
        v = row.get("offcpu_ms")
        t.check(isinstance(v, (int, float)) and not isinstance(v, bool),
                f"[sessions cpu] pid {row.get('pid')} carries a numeric "
                f"offcpu_ms (got {v!r})")
    # The response-level total is the sum over the rows, which is what lets a
    # client account for the time without walking them.
    total = sum(r.get("offcpu_ms", 0.0) for r in rows)
    t.check(abs(total - real.get("offcpu_ms", -1.0)) < 1e-6,
            "[sessions cpu] response offcpu_ms == the sum over rows "
            f"(got {real.get('offcpu_ms')!r}, rows sum to {total!r})")


# ── #333: a corrupt trace must not answer as an empty one ───────────────────
#
# Lives here because this is the suite that owns "what the real server's
# responses are allowed to look like", and because a truncated file needs a
# real pgwt-server reading real bytes -- the mock cannot have the bug.
#
# THE DEFECT. Truncate current.trace so the coverage layer can no longer read
# it and every raw-path command answers as though the window were simply
# empty: `{"fidelity": "none", "db_time_ms": 0, "rows": []}`. The UI renders
# fidelity "none" as a "No data" badge, so this is a LABELLED absence, not a
# silent zero -- but "no data" and "the file holding your data is corrupt"
# are different facts and the operator cannot tell them apart. Two separate
# review rounds on agent/block-transition-aggregate mistook exactly this for
# a fidelity regression.
#
# WHAT THIS PINS, and the ways it could be satisfied without checking
# anything (each has its own case below):
#   - the baseline: the SAME dir, before truncation, answers with data. A
#     refusal that fired on the healthy fixture too would prove nothing.
#   - an EMPTY trace directory is still "no data", never "corrupt". A gate
#     that refuses on absence has stopped distinguishing the two facts, which
#     is the whole point of the issue.
#   - a trace whose header names an UNSUPPORTED VERSION stays SKIPPED, as
#     src/event_reader.c documents, rather than becoming a corruption
#     refusal: that is a deliberate forward-compatibility behaviour and
#     turning it into an outage would be a regression dressed as a fix.
#   - every raw-path command refuses, not just the one that was checked.
#
# Nothing here is timed: a server is started, three or four requests are
# sent, the server is stopped.

CORRUPT_CMDS = ("top_events", "time_model", "top_sessions", "top_queries",
                "transitions")


def _corrupt_probe(t, trace_dir, label, want_refusal, **qargs):
    """Query every raw command and report how each one answered."""
    with ServerHarness(trace_dir) as srv:
        for cmd in CORRUPT_CMDS:
            r = srv.query(cmd, **qargs)
            code = r.get("code")
            if want_refusal:
                t.check(code == "trace_unreadable",
                        f"[{label}] {cmd} refuses with code "
                        f"'trace_unreadable' (got {code!r}, "
                        f"fidelity={r.get('fidelity')!r})")
                t.check(isinstance(r.get("hint"), str) and r.get("hint"),
                        f"[{label}] {cmd} refusal carries a hint")
                t.check("unreadable_file" in r,
                        f"[{label}] {cmd} refusal names the file it could "
                        f"not read")
            else:
                t.check(code != "trace_unreadable",
                        f"[{label}] {cmd} is NOT reported as corrupt "
                        f"(got {code!r})")


def check_corrupt_trace_refusal(t):
    import shutil

    # 1. Control: the untouched fixture answers with data, and no command
    #    claims corruption.
    trace_dir = generate_traces(SCENARIO)
    try:
        with ServerHarness(trace_dir) as srv:
            r = srv.query("top_events")
            t.check(r.get("fidelity") == "exact" and r.get("db_time_ms", 0) > 0,
                    "[corrupt trace] control: the intact fixture answers "
                    f"with data (fidelity={r.get('fidelity')!r}, "
                    f"db_time_ms={r.get('db_time_ms')!r})")
        _corrupt_probe(t, trace_dir, "corrupt trace/intact", False)

        # 2. The defect: truncate current.trace to a third of its length,
        #    leaving current.trace.meta claiming its committed blocks.
        path = os.path.join(trace_dir, "current.trace")
        size = os.path.getsize(path)
        with open(path, "r+b") as f:
            f.truncate(size // 3)
        t.check(os.path.getsize(path) < size,
                "[corrupt trace] the fixture file really was truncated")
        _corrupt_probe(t, trace_dir, "corrupt trace/truncated", True)
    finally:
        cleanup_traces(trace_dir)

    # 2b. The OTHER corruption shape, and the other half of the fix: cut the
    #     file so short that the reader cannot open it at all (no block index
    #     by any of its three strategies) while current.trace.meta still
    #     claims a committed block. 2 above exercises the decode counter in
    #     the loader; this one exercises the coverage layer's own refusal.
    #     Without it that detector is code no test reaches.
    short_dir = generate_traces(SCENARIO)
    try:
        path = os.path.join(short_dir, "current.trace")
        with open(path, "r+b") as f:
            f.truncate(30)          # header (28 B) + 2 stray bytes
        with open(path + ".meta") as f:
            t.check(int(f.read().strip()) >= 1,
                    "[corrupt trace] the meta still claims a committed block")
        _corrupt_probe(t, short_dir, "corrupt trace/unopenable", True)
    finally:
        cleanup_traces(short_dir)

    # 3. A capture that has committed NOTHING yet is absence, not corruption:
    #    header only, and a meta that says zero blocks. The reader cannot open
    #    such a file either (no block index by any of its three strategies),
    #    so this is the case that separates "the reader refused" from "the
    #    bytes are damaged" -- a detector keyed on the refusal alone would
    #    call this corrupt and report an outage on an idle capture.
    #    (A trace directory with NO files at all cannot be probed: pgwt-server
    #    exits at startup with "no trace files found", which is already a
    #    loud and separate refusal.)
    fresh_dir = generate_traces(SCENARIO)
    try:
        path = os.path.join(fresh_dir, "current.trace")
        with open(path, "r+b") as f:
            f.truncate(28)          # sizeof(struct pgwt_trace_file_header)
        with open(path + ".meta", "w") as f:
            f.write("0\n")
        _corrupt_probe(t, fresh_dir, "corrupt trace/nothing committed", False)
    finally:
        cleanup_traces(fresh_dir)

    # 3b. A window with no events in it is absence too, on a perfectly
    #     readable file.
    empty_window = generate_traces(SCENARIO)
    try:
        _corrupt_probe(t, empty_window, "corrupt trace/empty window", False,
                       from_=BASE + 10 ** 12, to_=BASE + 2 * 10 ** 12)
    finally:
        cleanup_traces(empty_window)

    # 4. An unsupported trace VERSION stays skipped, not refused. The header
    #    is `magic u32 | version u16 | ...` (src/event_writer.h), so writing
    #    a version the reader does not know is a 2-byte patch at offset 4.
    future_dir = generate_traces(SCENARIO)
    try:
        path = os.path.join(future_dir, "current.trace")
        with open(path, "r+b") as f:
            f.seek(4)
            f.write((99).to_bytes(2, "little"))
        _corrupt_probe(t, future_dir, "corrupt trace/future version", False)
    finally:
        cleanup_traces(future_dir)


def check_aas_fidelity_ranges(t, srv):
    """#322/#341: `fidelity_ranges` on the aas response, asserted POSITIVELY
    against the real server.

    The schema comparison above is SYMMETRIC: if both the real server and the
    mock omitted this field it would pass clean -- which is exactly the state
    this change exists to leave behind (the consumer in
    web/static/lib/builders/fidelity.js has read the field since B5 while
    `grep -c fidelity_ranges src/server.c` returned 0). So the real server's
    own response is checked here, independently of the mock.

    Non-vacuous on THIS fixture even though it is transitions-only: the
    server expands the default window well past the fixture's ~40ms of
    events, so an honest coverage-derived partition has "none" ranges around
    one "exact" range that is STRICTLY INSIDE the window. A field stamped
    from the window bounds -- one whole-window entry -- fails both of the
    last two checks. The multi-fidelity case (more than one range, two
    tokens, the boundary at a pinned timestamp) needs a window that spans a
    fidelity change and lives in tests/test_data_fidelity.py's
    test_fidelity_ranges, which has the mixed fixture.
    """
    real = srv.query("aas")
    t.check("error" not in real,
            "[fidelity ranges] real server answered without error"
            + (f" (got: {real.get('error')})" if "error" in real else ""))
    if "error" in real:
        return
    ranges = real.get("fidelity_ranges")
    t.check(isinstance(ranges, list) and len(ranges) > 0,
            f"[fidelity ranges] aas carries a non-empty fidelity_ranges "
            f"(got {ranges!r})")
    if not isinstance(ranges, list) or not ranges:
        return
    for i, r in enumerate(ranges):
        t.check(isinstance(r, dict) and
                set(r.keys()) == {"from", "to", "fidelity"},
                f"[fidelity ranges] range {i} keys are from/to/fidelity "
                f"(got {r!r})")
        if not isinstance(r, dict):
            continue
        for key in ("from", "to"):
            v = r.get(key)
            t.check(isinstance(v, int) and not isinstance(v, bool),
                    f"[fidelity ranges] range {i} {key} is an integer ns "
                    f"value, matching the bucket `t` domain (got {v!r})")
        t.check(r.get("fidelity") in ("exact", "sampled", "mixed", "none"),
                f"[fidelity ranges] range {i} fidelity is a known token "
                f"(got {r.get('fidelity')!r})")

    buckets = real.get("buckets", [])
    t.check(len(buckets) > 0, "[fidelity ranges] aas returned buckets")
    if buckets and isinstance(real.get("bucket_ns"), int):
        win_from = buckets[0]["t"]
        win_to = buckets[-1]["t"] + real["bucket_ns"]
        t.check(ranges[0].get("from") == win_from and
                ranges[-1].get("to") == win_to,
                f"[fidelity ranges] the partition spans the plotted window "
                f"({ranges[0].get('from')}..{ranges[-1].get('to')} vs "
                f"{win_from}..{win_to})")
        gaps = [(ranges[i]["to"], ranges[i + 1]["from"])
                for i in range(len(ranges) - 1)
                if ranges[i]["to"] != ranges[i + 1]["from"]]
        t.check(not gaps,
                f"[fidelity ranges] contiguous, no gap or overlap ({gaps})")
        exact = [r for r in ranges if r.get("fidelity") == "exact"]
        t.check(len(exact) >= 1,
                f"[fidelity ranges] the transition fixture yields an exact "
                f"range (got {[r.get('fidelity') for r in ranges]})")
        t.check(any(r.get("fidelity") == "none" for r in ranges),
                f"[fidelity ranges] the uncovered part of the expanded window "
                f"is reported as 'none', not folded into the data range "
                f"(got {[r.get('fidelity') for r in ranges]})")
        t.check(all(r["from"] > win_from or r["to"] < win_to for r in exact),
                f"[fidelity ranges] the exact range is strictly inside the "
                f"window -- derived from COVERAGE, not stamped from the "
                f"window bounds (got {exact!r}, window {win_from}..{win_to})")

    kinds = {r.get("fidelity") for r in ranges}
    derived = ("mixed" if {"exact", "sampled"} <= kinds
               else "exact" if "exact" in kinds
               else "sampled" if "sampled" in kinds else "none")
    t.check(derived == real.get("fidelity"),
            f"[fidelity ranges] the ranges imply the same window token the "
            f"response reports (derived {derived!r} vs "
            f"{real.get('fidelity')!r})")


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
            check_transitions_idle_flag(t, srv)
            check_protocol_revision(t, srv)
            check_top_sessions_cpu_fields(t, srv)
            check_aas_fidelity_ranges(t, srv)
    finally:
        cleanup_traces(trace_dir)

    # #333: its own trace dirs (one of them deliberately corrupt), so it runs
    # after the schema comparison above has had an intact fixture.
    check_corrupt_trace_refusal(t)

    ok = t.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
