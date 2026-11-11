#!/usr/bin/env python3
"""test_data_sessions.py — Per-session attribution.

Verifies that top_sessions correctly attributes time to each PID.
"""
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, ACTIVITY_IDLE,
)
import summary_raw_parity_lib as parity

BASE_TS = 10_000_000_000_000
S = 1_000_000_000


def row_scenario():
    """The middle 120s has two active pids, one idle-only pid, and a pid
    active ONLY in the neighbouring window. Every interval stays inside
    its closing second, so this tests membership, not boundary arithmetic.
    Equal active totals also exercise the pid tie-break in the row order.
    """
    events = []
    for second in (0, 60, 120, 179, 180, 239):
        for pid in (1000, 1001, 9000, 9001):
            active = pid in (1000, 1001) or (pid == 9001 and second < 60)
            events.append({"pid": pid, "ts": BASE_TS + second * S + S // 2,
                           "dur": 10_000_000,
                           "old": IO_DATA_FILE_READ if active else ACTIVITY_IDLE,
                           "new": CPU, "qid": 0})
    return {"backends": [{"pid": pid, "type": "bgwriter"}
                          for pid in (1000, 1001, 9000, 9001)],
            "events": events}


def require_pids(response, expected):
    if "error" in response:
        raise AssertionError(response)
    actual = [row["pid"] for row in response["rows"]]
    if actual != expected:
        raise AssertionError(f"pids {actual}, expected {expected}")


def test_row_routes(t):
    trace_dir = generate_traces(row_scenario())
    try:
        with tempfile.TemporaryDirectory(prefix="pgwt_session_rows_") as scratch:
            raw_dir = os.path.join(scratch, "raw")
            stale_dir = os.path.join(scratch, "v11")
            shutil.copytree(trace_dir, raw_dir)
            shutil.copytree(trace_dir, stale_dir)
            for path in Path(raw_dir).glob("*.summary*"):
                path.unlink()
            # Real v11 output from origin/master (4c2f6b87), including the
            # pid-only idle rows. Raw files come from the same scenario.
            fixture = Path(__file__).parent / "fixtures/session_rows_v11"
            if struct.unpack_from("=IH", (fixture / "current.summary").read_bytes()) != (
                    0x53574750, 11):
                raise AssertionError("legacy fixture must be an actual v11 summary")
            for name in ("current.summary", "current.summary.meta"):
                shutil.copyfile(fixture / name, Path(stale_dir) / name)

            windows = (("middle", 60, 180, [1000, 1001]),
                       ("neighbour", 0, 120, [1000, 1001, 9001]))
            for route, directory, expected_route in (
                    ("raw", raw_dir, parity.ROUTE_RAW),
                    ("summary", trace_dir, parity.ROUTE_SUMMARY),
                    ("v11 fallback", stale_dir, parity.ROUTE_RAW)):
                for window, lo, hi, expected in windows:
                    bounds = {"from_": BASE_TS + lo * S,
                              "to_": BASE_TS + hi * S}
                    err = os.path.join(scratch, "stderr")
                    with ServerHarness(directory, stderr_path=err) as srv:
                        for limit in (None, 1, 10):
                            args = {} if limit is None else {"limit": limit}
                            answer = srv.query("top_sessions", **bounds, **args)
                            # top_sessions has no top-N cap on EITHER route.
                            # limit belongs to other commands, and 1 must
                            # still return BOTH active rows here.
                            try:
                                require_pids(answer, expected)
                            except AssertionError as exc:
                                t.check(False, f"{route}/{window}/limit={limit}: {exc}")
                            else:
                                t.check(True, f"{route}/{window}/limit={limit}: {expected}")
                        # A PID filter independently forces raw and tests
                        # that the excluded pid really has no active row.
                        idle = srv.query("top_sessions", **bounds,
                                         filters={"pid": 9000})
                        t.check_eq(idle.get("rows"), [], f"{route}: idle pid filter")
                    normal_stderr = Path(err).read_text()
                    with ServerHarness(directory, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
                        oracle = srv.query("time_model", **bounds)
                    actual_route = parity.classify_route(oracle, normal_stderr)
                    t.check_eq(actual_route, expected_route, f"{route}/{window}: route proof")
                    if route == "v11 fallback":
                        t.check("not summary v" in normal_stderr,
                                "old summaries refused by accounting version")

            # Negative controls: each detector must reject the exact shape
            # it is intended to catch, including extra rows at zero DB Time.
            for bad in ([1000, 1001, 9000], [1000], [1001, 1000]):
                try:
                    require_pids({"rows": [{"pid": p} for p in bad]}, [1000, 1001])
                except AssertionError:
                    t.check(True, f"row detector rejects {bad}")
                else:
                    t.check(False, f"row detector accepted {bad}")
            t.check_eq(parity.classify_route(
                {"code": "window_too_large", "max_events": 1}, ""), parity.ROUTE_UNKNOWN,
                       "route detector rejects missing fallback evidence")
            # Extend the parity gate through its public extraction/comparison
            # core without editing the two files owned by another branch.
            good = {"rows": [{"pid": 1000, "db_time_ms": 0}]}
            for bad in ({"rows": []}, {"rows": good["rows"] + [
                    {"pid": 9000, "db_time_ms": 0}]}):
                findings = parity.compare(parity.extract_sessions(good),
                                          parity.extract_sessions(bad), {})
                t.check(any(f.field == "sessions/rowset" and f.status == parity.NEW
                            for f in findings), "parity core rejects changed session row set")
            t.check(all(f.status == parity.AGREE for f in parity.compare(
                parity.extract_sessions(good), parity.extract_sessions(good), {})),
                "parity core accepts identical zero-time non-idle rows")
    finally:
        cleanup_traces(trace_dir)


def build_scenario():
    """3 PIDs with known DB Time.

    PID 1000: CPU 3ms + IO 7ms = 10ms DB Time
    PID 1001: Lock 5ms = 5ms DB Time
    PID 1002: Activity(idle) 10ms = 0 DB Time (excluded)
    """
    events = []
    ts = BASE_TS

    # PID 1000
    ts += 3_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 3_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})
    ts += 7_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 7_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    # PID 1001
    ts2 = BASE_TS + 500_000
    ts2 += 5_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 5_000_000,
                    "old": LOCK_RELATION, "new": CPU, "qid": 200})

    # PID 1002 (idle — excluded)
    ts3 = BASE_TS + 200_000
    ts3 += 10_000_000
    events.append({"pid": 1002, "ts": ts3, "dur": 10_000_000,
                    "old": ACTIVITY_IDLE, "new": CPU, "qid": 0})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "bgwriter"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "events": events,
    }


def test_zero_time_non_idle_row(t):
    """#852: zero DB Time alone must not exclude a non-idle session."""
    # Three 10ms closes for pid 1000 in [60,180); the other closes keep
    # the window inside the capture. Pid 7000 has ONLY one zero-duration
    # IO close at 100.5s, so nothing else can give it a session row.
    events = [{"pid": 1000, "ts": BASE_TS + second * S + S // 2,
               "dur": 10_000_000, "old": IO_DATA_FILE_READ,
               "new": CPU, "qid": 0}
              for second in (0, 60, 120, 179, 180, 239)]
    events.append({"pid": 7000, "ts": BASE_TS + 100 * S + S // 2,
                   "dur": 0, "old": IO_DATA_FILE_READ, "new": CPU, "qid": 0})
    scenario = {"backends": [{"pid": pid, "type": "bgwriter"}
                              for pid in (1000, 7000)],
                "events": sorted(events, key=lambda event: event["ts"])}
    expected_rows = [(1000, 30.0), (7000, 0.0)]  # (pid, DB Time in ms)
    bounds = {"from_": BASE_TS + 60 * S, "to_": BASE_TS + 180 * S}
    trace_dir = generate_traces(scenario)
    try:
        with tempfile.TemporaryDirectory(prefix="pgwt_zero_time_row_") as scratch:
            raw_dir = os.path.join(scratch, "raw")
            shutil.copytree(trace_dir, raw_dir)
            for path in Path(raw_dir).glob("*.summary*"):
                path.unlink()
            route_rows = {}
            for route, directory, expected_route in (
                    ("raw", raw_dir, parity.ROUTE_RAW),
                    ("summary", trace_dir, parity.ROUTE_SUMMARY)):
                err = os.path.join(scratch, "stderr")
                with ServerHarness(directory, stderr_path=err) as srv:
                    answer = srv.query("top_sessions", **bounds)
                t.check("error" not in answer,
                        f"zero-time-non-idle/{route}: successful response")
                route_rows[route] = [(row["pid"], row["db_time_ms"])
                                     for row in answer.get("rows", [])]
                t.check_eq(route_rows[route], expected_rows,
                           f"zero-time-non-idle/{route}: pid 7000 retained at 0ms")
                with ServerHarness(directory, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
                    oracle = srv.query("top_sessions", **bounds)
                t.check_eq(parity.classify_route(oracle, Path(err).read_text()),
                           expected_route, f"zero-time-non-idle/{route}: route proof")
            t.check_eq(route_rows["summary"], route_rows["raw"],
                       "zero-time-non-idle: routes have identical row sets and order")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_sessions")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_sessions")
            rows = resp.get("rows", [])
            by_pid = {r["pid"]: r for r in rows}

            print("--- PID 1000: 10ms DB Time ---")
            s1 = by_pid.get(1000, {})
            t.check_approx(s1.get("db_time_ms", -1), 10.0, 0.001,
                           "PID 1000 DB Time = 10ms")

            print("--- PID 1001: 5ms DB Time ---")
            s2 = by_pid.get(1001, {})
            t.check_approx(s2.get("db_time_ms", -1), 5.0, 0.001,
                           "PID 1001 DB Time = 5ms")

            print("--- PID 1002: idle, excluded ---")
            t.check(1002 not in by_pid, "PID 1002 not in output (idle excluded)")

            print("--- Ordering ---")
            if len(rows) >= 2:
                t.check(rows[0]["db_time_ms"] >= rows[1]["db_time_ms"],
                        "Sessions sorted by DB Time descending")

    finally:
        cleanup_traces(trace_dir)

    test_row_routes(t)
    test_zero_time_non_idle_row(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
