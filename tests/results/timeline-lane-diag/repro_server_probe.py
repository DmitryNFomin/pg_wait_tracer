#!/usr/bin/env python3
"""Diagnosis evidence for the Timeline single-lane finding (#timeline-lane-diag).

Not a committed unit test -- a one-off repro script kept alongside its output
as evidence. Exercises the REAL locally-built pgwt-server (BPF-free,
demos/build-server-local.sh) against a synthetic 5-PID trace, the same shape
as tests/test_data_timeline.py::check_many_pids, but printing the raw counts
instead of asserting, so the numbers are visible in the transcript.

Run from the repo root:
    bash demos/build-server-local.sh
    cd tests && make gen_test_traces CC=cc \
        CFLAGS="-g -O2 -Wall -I../src -I../include \
        -include ../demos/replay-compat.h -I$(brew --prefix)/include \
        -L$(brew --prefix)/lib"
    python3 results/timeline-lane-diag/repro_server_probe.py
"""
import json
import os
import sys

TESTS_DIR = os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, TESTS_DIR)
from server_harness import ServerHarness, generate_traces, cleanup_traces
from test_data_timeline import build_many_pids, BASE_TS, WINDOW_END

N = 5
scenario, pids = build_many_pids(N)
trace_dir = generate_traces(scenario)
try:
    with ServerHarness(trace_dir) as srv:
        resp = srv.query("session_timeline", timeout=5,
                          from_=BASE_TS, to_=WINDOW_END,
                          filters={"query_id": "100"})
        print("query_id-only filter (no pid): expected distinct PIDs =", N)
        print("  response pids:", resp.get("pids"))
        print("  response pid count:", len(resp.get("pids", [])))
        print("  response bar count:", len(resp.get("events", [])))
        print("  response total_count:", resp.get("total_count"))
        print("  response truncated:", resp.get("truncated"))
        with open(os.path.join(os.path.dirname(__file__),
                                "real_server_response.json"), "w") as f:
            json.dump(resp, f, indent=2)

        last = srv.query("session_timeline", timeout=5,
                          from_=BASE_TS, to_=WINDOW_END,
                          filters={"pid": pids[-1]})
        print()
        print("pid-filtered request for pid", pids[-1], "(single-session drilldown):")
        print("  response pids:", last.get("pids"))
        print("  response bar count:", len(last.get("events", [])))
finally:
    cleanup_traces(trace_dir)
