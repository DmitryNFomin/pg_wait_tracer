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


# ── Sampled/Mixed fidelity coverage (#306/#321 "mock-underread" follow-up) ───
# Every COMMANDS run above uses an all-TRANSITIONS fixture, so the real
# server's response is always "exact": before this, test_protocol_drift had
# ZERO fidelity references (verified: `grep -c fidelity` on the pre-change
# file was 0) and could not catch the mock inventing protocol under
# PGWT_MOCK_FIDELITY=sampled/mixed -- e.g. a "fidelity_ranges" array
# production does not emit (see the issue text; src/server.c's add_fidelity
# emits only the scalar "fidelity" + "sample_period_ns"). gen_test_traces
# already supports a "samples"/"sample_period_ns" scenario (trace format v2,
# used by tests/test_data_fidelity.py), so this is mechanically achievable.

FID_BASE = 20_000_000_000_000   # separate time range from SCENARIO above
FID_PERIOD = 100_000_000        # 100ms = 10 Hz, matches the mock's default
                                 # PGWT_MOCK_SAMPLE_PERIOD_NS


def _fid_sample(pid, ts, event, qid=0):
    return {"pid": pid, "ts": ts, "event": event, "qid": qid}


def _build_samples(base):
    """A SAMPLES run with a bit of everything (CPU, IO, Lock, LWLock) across
    two pids/queries, used standalone (-> PGWT_FIDELITY_SAMPLED, no
    transitions anywhere) and appended to SCENARIO (-> PGWT_FIDELITY_MIXED,
    see SCENARIO_MIXED below)."""
    samples = []
    ts = base
    for _ in range(10):
        ts += FID_PERIOD
        samples.append(_fid_sample(2000, ts, IO_DATA_FILE_READ, 300))
    for _ in range(6):
        ts += FID_PERIOD
        samples.append(_fid_sample(2000, ts, LOCK_TXN, 300))
    for _ in range(8):
        ts += FID_PERIOD
        samples.append(_fid_sample(2001, ts, CPU, 400))
    for _ in range(4):
        ts += FID_PERIOD
        samples.append(_fid_sample(2001, ts, LWLOCK_WAL_WRITE, 400))
    samples.sort(key=lambda s: s["ts"])
    return samples


SCENARIO_SAMPLED = {
    "backends": [
        {"pid": 2000, "type": "client", "user": "u", "db": "d"},
        {"pid": 2001, "type": "client", "user": "u", "db": "d"},
    ],
    "queries": [{"id": 300, "text": "SELECT 2"}, {"id": 400, "text": "SELECT 3"}],
    "sample_period_ns": FID_PERIOD,
    "samples": _build_samples(FID_BASE),
}

# Mixed: SCENARIO's own all-transitions events, PLUS a disjoint samples run
# well after them -- both has_transitions and has_samples are true for the
# default (whole-capture) window, so load_fidelity reports
# PGWT_FIDELITY_MIXED (src/compute.h pgwt_fidelity_of).
SCENARIO_MIXED = dict(SCENARIO)
SCENARIO_MIXED["sample_period_ns"] = FID_PERIOD
SCENARIO_MIXED["samples"] = _build_samples(FID_BASE + 100_000_000_000)

# EXACT-required commands whose "unavailable" gate the mock's _EXACT_VIEWS
# ALREADY matches src/server.c for (each of these five calls
# pgwt_fidelity_unavailable(PGWT_REQ_EXACT, ...) in both
# handle_heatmap/handle_transitions/handle_concurrency/handle_lock_chains/
# handle_interference AND is listed in mock_server._EXACT_VIEWS).
# session_timeline, executions, execution_detail, exec_scatter,
# fingerprints and variants are DELIBERATELY excluded here: src/server.c
# gates session_timeline/fingerprints/variants as EXACT-required but
# mock_server._EXACT_VIEWS does not, and the reverse for
# executions/exec_scatter (gated in the mock, NOT in src/server.c) -- a
# real, pre-existing mismatch this task did not introduce. Widening
# _EXACT_VIEWS to "fix" it would also flip test_web_ui.py's
# test_b5_unavailable_panel (which asserts the waterfall/scatter tabs --
# executions/exec_scatter -- show the unavailable panel today), so it is
# reported, not patched, here.
#
# "info" and "top_queries" are ALSO excluded, for reasons this gate itself
# found (ran once, red, before these exclusions -- see the report):
#   - "info" never carries a "fidelity" field at all (it is a capture-wide
#     header, not a per-window view) -- asserting one here was a bug in
#     THIS harness, not a mock/real mismatch.
#   - "top_queries" rows carry lifecycle fields (exec_count, avg_exec_ms,
#     plan_count, ...) ONLY when EXEC_START/END/PLAN_START/END markers
#     exist, which are transitions -- a samples-only window never has them.
#     src/server.c's real response omits the fields entirely there;
#     mock_server.py's canned _CANNED["top_queries"] always includes them,
#     unconditionally. That IS a real, separate pre-existing protocol gap in
#     the mock (lifecycle fields are not fidelity-gated at all) -- distinct
#     from the CPU-under-read this task fixes, reported rather than
#     silently absorbed into this task's diff.
FIDELITY_COMMANDS = [
    ("time_model",          "time_model",       {}),
    ("aas",                 "aas",              {"buckets": 30}),
    ("aas detail=events",   "aas",              {"buckets": 30, "detail": "events"}),
    ("top_events",          "top_events",       {}),
    ("top_events class=io", "top_events",       {"filters": {"class": "io"}}),
    ("top_sessions",        "top_sessions",     {}),
    ("heatmap",             "heatmap",          {}),
    ("transitions",         "transitions",      {}),
    ("concurrency",         "concurrency",      {"buckets": 30}),
    ("lock_chains",         "lock_chains",      {}),
    ("interference",        "interference",     {}),
]


def run_fidelity_drift(t, label, scenario, mock_fidelity):
    """Same schema-drift check as main()'s loop, but against a
    samples-bearing fixture with the mock pinned (mock_server._FID.fidelity,
    not the env var -- the module is already imported in-process, same
    pattern test_b5_* uses via a subprocess env var, here done in-process
    since this file calls handle_request() directly) to the matching
    fidelity mode, so a mock that invented a field under
    PGWT_MOCK_FIDELITY=sampled/mixed fails here exactly like a drifted
    exact-mode field fails above."""
    trace_dir = generate_traces(scenario)
    prev_fidelity = mock_server._FID.fidelity
    mock_server._FID.fidelity = mock_fidelity
    try:
        with ServerHarness(trace_dir) as srv:
            for cmd_label, cmd, kwargs in FIDELITY_COMMANDS:
                full_label = f"{label}/{cmd_label}"
                real = srv.query(cmd, **kwargs)
                mock = mock_query(cmd, req_id=real.get("id", 0), **kwargs)

                t.check("error" not in real,
                        f"[{full_label}] real server answered without error"
                        + (f" (got: {real.get('error')})" if "error" in real else ""))
                t.check("error" not in mock,
                        f"[{full_label}] mock answered without error"
                        + (f" (got: {mock.get('error')})" if "error" in mock else ""))
                if "error" in real or "error" in mock:
                    continue

                # The real server's actual fidelity tag must match what we
                # asked the mock to pretend -- otherwise the fixture itself
                # is wrong (e.g. the samples run did not land where expected)
                # and the schema comparison below would silently compare two
                # different kinds of window.
                t.check(real.get("fidelity") == mock_fidelity,
                        f"[{full_label}] real server fixture is "
                        f"'{mock_fidelity}' fidelity "
                        f"(got '{real.get('fidelity')}')")

                diffs = []
                diff_schemas(schema_of(real), schema_of(mock), "", diffs)
                diffs = filter_allowed(diffs)
                t.check(not diffs, f"[{full_label}] response schemas match")
                for d in diffs:
                    print(f"    DRIFT: {d}")
    finally:
        mock_server._FID.fidelity = prev_fidelity
        cleanup_traces(trace_dir)


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

    run_fidelity_drift(t, "sampled", SCENARIO_SAMPLED, "sampled")
    run_fidelity_drift(t, "mixed", SCENARIO_MIXED, "mixed")

    ok = t.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
