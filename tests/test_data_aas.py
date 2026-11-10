#!/usr/bin/env python3
"""test_data_aas.py — 0B.2: AAS bucket correctness (0% tolerance).

Generates 4 backends each active for exactly 5 seconds, verifies AAS ≈ 4.0.
Also verifies per-class AAS breakdown sums correctly.
Pins max_aas to the full bucket total when the event stack is truncated.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, LWLOCK_WAL_WRITE,
    CLASS_IO, make_event_id,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """4 PIDs, each with events covering exactly 5 seconds.

    PID 1000: 5s of CPU
    PID 1001: 5s of IO:DataFileRead
    PID 1002: 5s of Lock:relation
    PID 1003: 5s of LWLock:WALWrite

    Time range: BASE_TS to BASE_TS + 5_000_000_000
    Expected total AAS over 5s = 4.0 (4 active sessions)
    """
    events = []
    pids = [1000, 1001, 1002, 1003]
    wait_events = [CPU, IO_DATA_FILE_READ, LOCK_RELATION, LWLOCK_WAL_WRITE]

    # Each PID gets one big event spanning 5 seconds
    for i, (pid, we) in enumerate(zip(pids, wait_events)):
        ts = BASE_TS + 5_000_000_000
        events.append({
            "pid": pid,
            "ts": ts,
            "dur": 5_000_000_000,
            "old": we,
            "new": CPU if we != CPU else IO_DATA_FILE_READ,
            "qid": 100 + i,
        })

    return {
        "backends": [
            {"pid": p, "type": "client", "user": "test", "db": "testdb"}
            for p in pids
        ],
        "queries": [{"id": 100 + i, "text": f"SELECT {i}"} for i in range(4)],
        "events": events,
    }


def check_event_peak(t, sessions):
    """Top-16 truncation must not change the peak; 16 is the control."""
    duration = 5_000_000_000
    scenario = {
        "backends": [
            {"pid": 2000 + i, "type": "client", "user": "test", "db": "testdb"}
            for i in range(sessions)
        ],
        "events": [
            {"pid": 2000 + i, "ts": BASE_TS + duration, "dur": duration,
             "old": make_event_id(CLASS_IO, i), "new": CPU}
            for i in range(sessions)
        ],
    }
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            window = {"from_": BASE_TS, "to_": BASE_TS + duration,
                      "buckets": 1, "filters": {"class": "io"}}
            classes = srv.query("aas", **window)
            events = srv.query("aas", detail="events", **window)

            print(f"--- {sessions} simultaneous distinct IO events ---")
            t.check_eq(len(classes["buckets"]), 1, "One class bucket")
            total = sum(v for k, v in classes["buckets"][0].items()
                        if k not in ("t", "total", "cat"))
            t.check_eq(total, sessions, f"Class bucket total = {sessions}")
            t.check_eq(classes["max_aas"], sessions,
                       f"Class max_aas = {sessions}")
            t.check_eq(events["max_aas"], sessions,
                       f"Event-detail max_aas = {sessions}")
            t.check_eq(events.get("breakdown"), "events", "Event breakdown returned")
            t.check_eq(len(events["series"]), 16, "16 event series returned")
            t.check_eq(len({s["event_id"] for s in events["series"]}), 16,
                       "16 distinct event IDs returned")
            t.check_eq(len(events["buckets"]), 1, "One event bucket")
            stack = events["buckets"][0]["aas"]
            t.check_eq(len(stack), 16, "16 event bucket values returned")
            t.check_eq(sum(stack), 16, "Returned event stack total = 16")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_aas")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # Query AAS with 1 bucket covering the full range
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5_000_000_000)

            print("--- AAS total ---")
            buckets = resp.get("buckets", [])
            t.check(len(buckets) >= 1, f"Got {len(buckets)} AAS bucket(s)")

            if buckets:
                b = buckets[0]
                # Sum all class values (skip the T2 "cat" decomposition —
                # it is a parallel breakdown of the same time, not a class)
                total_aas = sum(v for k, v in b.items()
                                if k not in ("t", "total", "cat"))
                t.check_approx(total_aas, 4.0, 0.05,
                               "Total AAS = 4.0 (4 active sessions)")

                # T2: the category decomposition covers the same total
                # (all backends here are foreground clients; no io_workers)
                cat = b.get("cat", {})
                t.check(isinstance(cat, dict) and "execution" in cat,
                        "bucket carries the T2 category decomposition")
                cat_total = sum(cat.values())
                t.check_approx(cat_total, 4.0, 0.05,
                               "category AAS total = class AAS total")
                t.check_approx(cat.get("io_worker", -1), 0.0, 0.001,
                               "no io_worker AAS in a client-only scenario")

            # Also check time_model for AAS (use same 5s window)
            resp_tm = srv.query("time_model",
                                from_=BASE_TS, to_=BASE_TS + 5_000_000_000)
            rows = {r["name"]: r for r in resp_tm["rows"]}

            print("--- time_model AAS ---")
            db_aas = rows.get("DB Time", {}).get("aas", -1)
            t.check_approx(db_aas, 4.0, 0.05,
                           "time_model DB Time AAS = 4.0")

            # Per-class AAS
            cpu_aas = rows.get("CPU*", {}).get("aas", -1)
            t.check_approx(cpu_aas, 1.0, 0.05, "CPU AAS = 1.0")

            io_aas = rows.get("IO", {}).get("aas", -1)
            t.check_approx(io_aas, 1.0, 0.05, "IO AAS = 1.0")

            lock_aas = rows.get("Lock", {}).get("aas", -1)
            t.check_approx(lock_aas, 1.0, 0.05, "Lock AAS = 1.0")

            lwlock_aas = rows.get("LWLock", {}).get("aas", -1)
            t.check_approx(lwlock_aas, 1.0, 0.05, "LWLock AAS = 1.0")

    finally:
        cleanup_traces(trace_dir)

    check_event_peak(t, 17)
    check_event_peak(t, 16)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
