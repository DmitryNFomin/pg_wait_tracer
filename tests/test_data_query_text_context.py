#!/usr/bin/env python3
"""Stage 4 query-text source/context precedence through pgwt-server."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import ServerHarness, TestRunner, cleanup_traces, generate_traces

BASE = 10_000_000_000_000


def row_by_qid(response, qid):
    for row in response.get("rows", []):
        if str(row.get("query_id")) == str(qid):
            return row
    return {}


def main():
    t = TestRunner("test_data_query_text_context")
    scenario = {
        "backends": [{
            "pid": 1000, "type": "client", "user": "u", "db": "a",
            "databaseid": 5, "userid": 10,
        }, {
            "pid": 3000, "type": "client", "user": "u", "db": "a",
            "databaseid": 5, "userid": 10,
        }, {
            # A retained sidecar can contain two lifetimes for a reused PID.
            "pid": 3000, "type": "client", "user": "v", "db": "b",
            "databaseid": 6, "userid": 11,
        }],
        "queries": [
            {"id": 100, "databaseid": 5, "userid": 10,
             "source": "pgss", "text": "select $1"},
            # Same qid shape in another context must never cross the boundary.
            {"id": 200, "databaseid": 6, "userid": 11,
             "source": "full", "text": "SELECT 'other-context-secret'"},
            {"id": 300, "databaseid": 5, "userid": 10,
             "source": "synthetic", "text": "SELECT ?"},
            {"id": 400, "databaseid": 5, "userid": 10,
             "source": "full", "text": "SELECT 'known-only'"},
            {"id": 600, "databaseid": 5, "userid": 10,
             "source": "full", "text": "SELECT 'old-pid-lifetime'"},
            {"id": 700, "databaseid": 5, "userid": 10,
             "source": "full", "text": "SELECT 'context-a'"},
            {"id": 700, "databaseid": 6, "userid": 11,
             "source": "full", "text": "SELECT 'context-b'"},
        ],
        "events": [
            {"pid": 1000, "ts": BASE + 1_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 100},
            {"pid": 1000, "ts": BASE + 2_000_000, "dur": 1_000_000,
             "old": 0x0A000001, "new": 0, "qid": 200},
            {"pid": 1000, "ts": BASE + 3_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 300, "flags": 128},
            {"pid": 1000, "ts": BASE + 4_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 400},
            {"pid": 2000, "ts": BASE + 5_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 400},
            {"pid": 1001, "ts": BASE + 6_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 500},
            {"pid": 1001, "ts": BASE + 6_500_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 501},
            {"pid": 3000, "ts": BASE + 7_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 600},
            {"pid": 1000, "ts": BASE + 8_000_000, "dur": 0,
             "old": 0xFFFFFFF0, "new": 0xFFFFFFF0, "qid": 700},
            {"pid": 1000, "ts": BASE + 9_000_000, "dur": 1_000_000,
             "old": 0, "new": 0x0A000001, "qid": 700},
            {"pid": 1000, "ts": BASE + 10_000_000, "dur": 0,
             "old": 0xFFFFFFF1, "new": 0xFFFFFFF1, "qid": 700},
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            first = srv.query("top_queries")
            t.check(row_by_qid(first, 100).get("text") == "select $1",
                    "same-context pgss text resolves")
            t.check("text" not in row_by_qid(first, 200),
                    "known-context miss never falls back to another context")
            synth = row_by_qid(first, 300)
            t.check(synth.get("text") == "SELECT ?" and
                    synth.get("attribution_quality") == "pg13-synth-v1",
                    "synthetic source/quality is observable")
            t.check("text" not in row_by_qid(first, 400),
                    "unknown contributor makes a mixed row unaddressable")
            t.check("text" not in row_by_qid(first, 500),
                    "missing live backend context leaves text unavailable")
            t.check("text" not in row_by_qid(first, 600),
                    "conflicting reused-PID contexts fail closed")
            variants = srv.query("variants")
            variant_rows = variants.get("exec", {}).get("variants", [])
            variant = next((row for row in variant_rows
                            if str(row.get("top_query_id")) == "700"), {})
            t.check(variant and "query_text" not in variant,
                    "qid-only variant text is omitted when contexts disagree")

            # Append text and backend context while the same server process is
            # running; top_queries must notice both without an info request.
            path = os.path.join(trace_dir, "query_texts.jsonl")
            with open(path, "a", encoding="utf-8") as sidecar:
                sidecar.write(json.dumps({
                    "q": "100", "d": 5, "u": 10, "s": "full",
                    "t": "SELECT 42", "ts": 2_000_000_000,
                }) + "\n")
                sidecar.write(json.dumps({
                    "q": "500", "d": 5, "u": 10, "s": "pgss",
                    "t": "select $1", "ts": 2_000_000_001,
                }) + "\n")
            path = os.path.join(trace_dir, "backends.jsonl")
            with open(path, "a", encoding="utf-8") as sidecar:
                sidecar.write(json.dumps({
                    "pid": 1001, "type": "client", "user": "u", "db": "a",
                    "dbid": 5, "userid": 10,
                }) + "\n")
            path = os.path.join(trace_dir, "query_sources.jsonl")
            with open(path, "a", encoding="utf-8") as sidecar:
                sidecar.write(json.dumps({
                    "q": "501", "d": 5, "u": 10, "s": "synthetic",
                    "v": "pg13-synth-v1",
                }) + "\n")
            second = srv.query("top_queries")
            t.check(row_by_qid(second, 100).get("text") == "SELECT 42",
                    "long-lived server reloads async raw upgrade and raw wins")
            t.check(row_by_qid(second, 500).get("text") == "select $1",
                    "long-lived server reloads backend context with query text")
            source_only = row_by_qid(second, 501)
            t.check("text" not in source_only and
                    source_only.get("attribution_quality") == "pg13-synth-v1",
                    "source-only sidecar preserves durable synthetic quality")
    finally:
        cleanup_traces(trace_dir)
    return 0 if t.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
