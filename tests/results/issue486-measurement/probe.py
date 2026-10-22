#!/usr/bin/env python3
"""issue486-measurement/probe.py — #486 scoped-request cross-check.

NOT a test, NOT a gate, and NOT registered with any runner. It is a
re-runnable RECORD of the diagnosis done for #486 on branch fix/answer-contracts:
the plan asked for one frozen window/filter snapshot comparing the outgoing
request, the wire counts/rows/truncation, and the Queries tab's exec_count for
the same query id, reported BEFORE any code change.

Builds a small synthetic fixture: 5 pids, each with 3 EXEC_START/EXEC_END
pairs for one query id (15 executions total — matching the "Queries tab
credits 15" shape the owner's #486 comment asked to confirm). Sends:

  1. `executions` scoped with `filters: {"query_id": <id>}` (a single scoped
     request, per the owner's ask) and reads total_count/rows/truncated.
  2. `top_queries` and reads the same query id's `exec_count`.

against the SAME ground truth: the number of EXEC_START/EXEC_END pairs this
script itself authored into the fixture (not derived from any server
output).

CAVEAT (noted in review): this fixture's 15 executions stay under the
`executions` command's default limit (100), so `truncated` is always False
here. It does NOT exercise the `truncated=true` / ">limit" case the client's
disclosure guard at web/static/views/waterfall.js:383 keys on — that is a
separate, still-open check for whichever branch implements the #486 client
disclosure (branch E).

Needs pgwt-server + tests/gen_test_traces built (e.g. `demos/build-server-local.sh`
on macOS). Run from anywhere:
    python3 tests/results/issue486-measurement/probe.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, TESTS_DIR)

from server_harness import (  # noqa: E402
    ServerHarness, generate_traces, cleanup_traces, CPU, IO_DATA_FILE_READ,
)

MARKER_EXEC_START = 0xFFFFFFF0
MARKER_EXEC_END = 0xFFFFFFF1
BASE = 10_000_000_000_000
MS = 1_000_000
QID = 777
N_PIDS = 5
N_EXECS_PER_PID = 3


def marker(pid, qid, ts, m):
    return {"pid": pid, "ts": ts, "dur": 0, "old": m, "new": 0, "qid": qid}


def build_scenario():
    backends = []
    events = []
    ground_truth = 0
    for p_i in range(N_PIDS):
        pid = 900 + p_i
        backends.append({"pid": pid, "type": "client", "user": "u", "db": "d"})
        ts = BASE + p_i * 1000 * MS
        for e_i in range(N_EXECS_PER_PID):
            start = ts + e_i * 100 * MS
            events.append(marker(pid, QID, start, MARKER_EXEC_START))
            events.append({"pid": pid, "ts": start + 1 * MS, "dur": 1 * MS,
                           "old": IO_DATA_FILE_READ, "new": CPU, "qid": QID})
            events.append(marker(pid, QID, start + 5 * MS, MARKER_EXEC_END))
            ground_truth += 1
    events.sort(key=lambda e: e["ts"])
    scenario = {
        "backends": backends,
        "queries": [{"id": QID, "text": "SELECT heavy"}],
        "events": events,
    }
    return scenario, ground_truth


def measure():
    scenario, ground_truth = build_scenario()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            exec_resp = srv.query("executions", limit=100,
                                  filters={"query_id": QID})
            tq_resp = srv.query("top_queries")
    finally:
        cleanup_traces(trace_dir)

    tq_row = None
    for row in tq_resp.get("rows", []):
        if str(row.get("query_id")) == str(QID):
            tq_row = row
            break

    return {
        "ground_truth_exec_pairs": ground_truth,
        "outgoing_request": {"cmd": "executions", "limit": 100,
                             "filters": {"query_id": QID}},
        "executions_response": {
            "total_count": exec_resp.get("total_count"),
            "open_count": exec_resp.get("open_count"),
            "completed_count": exec_resp.get("completed_count"),
            "truncated": exec_resp.get("truncated"),
            "len_rows": len(exec_resp.get("rows", [])),
        },
        "top_queries_exec_count": tq_row.get("exec_count") if tq_row else None,
        "agreement": {
            "executions.total_count == ground_truth":
                exec_resp.get("total_count") == ground_truth,
            "top_queries.exec_count == ground_truth":
                (tq_row.get("exec_count") if tq_row else None) == ground_truth,
            "top_queries.exec_count == executions.total_count":
                (tq_row.get("exec_count") if tq_row else None)
                == exec_resp.get("total_count"),
        },
    }


def main():
    result = measure()
    print(json.dumps(result, indent=2))
    out_path = os.path.join(HERE, "results.json")
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print("\nwrote", out_path)


if __name__ == "__main__":
    main()
