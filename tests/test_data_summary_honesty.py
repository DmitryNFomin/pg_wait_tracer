#!/usr/bin/env python3
"""Summaries stay off: long EXACT windows read raw, preserve fidelity and
refuse at the event limit, even with current or retired summary files.
Sampled windows retain honest values and empty windows retain fidelity=none.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)
from summary_raw_parity_lib import classify_route, ROUTE_RAW

BASE = 10_000_000_000_000
S = 1_000_000_000
PERIOD = 1 * S      # 1 Hz sampling keeps the sample count small


def summary_format_version():
    """Read the writer's version; a missing definition must fail loudly."""
    header = Path(__file__).resolve().parents[1] / "src" / "summary_writer.h"
    match = re.search(r"^#define\s+PGWT_SUMMARY_VERSION\s+(\d+)\s*$",
                      header.read_text(), re.MULTILINE)
    if match is None:
        raise RuntimeError(f"PGWT_SUMMARY_VERSION missing from {header}")
    return int(match.group(1))


PGWT_SUMMARY_VERSION = summary_format_version()


def build_tiered_15min():
    """Default-tiered 'last 15 minutes': sampled data only (no escalation
    ever fired). 900 IO samples at 1 Hz = 900 s of estimated IO time."""
    samples = []
    ts = BASE
    for _ in range(900):
        ts += PERIOD
        samples.append({"pid": 1000, "ts": ts, "event": IO_DATA_FILE_READ,
                        "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "sample_period_ns": PERIOD,
        "samples": samples,
    }, BASE, ts


def test_tiered_15min(t):
    print("\n### 1. FID-2: 15-min tiered window returns the sampled data ###")
    scenario, lo, hi = build_tiered_15min()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            frm, to = lo - 10 * S, hi + 10 * S   # ~920 s >= 120 s threshold
            expected_ms = 900 * (PERIOD / 1e6)   # 900 samples * 1 s

            tm = srv.query("time_model", from_=frm, to_=to)
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_eq(tm.get("fidelity"), "sampled",
                       "time_model over sampled-only 15 min: fidelity=sampled")
            t.check_approx(rows.get("DB Time", {}).get("ms", -1),
                           expected_ms, 0.001,
                           f"DB Time = {expected_ms:.0f}ms (never silent-empty)")

            ev = srv.query("top_events", from_=frm, to_=to)
            t.check_eq(ev.get("fidelity"), "sampled",
                       "top_events fidelity=sampled")
            evrows = {r["name"]: r for r in ev["rows"]}
            t.check_approx(evrows.get("IO:DataFileRead", {}).get("total_ms", -1),
                           expected_ms, 0.001,
                           "top_events IO total from samples")
            # FID-3: latency columns are fabrications over sampled data —
            # they must be gated (null), not presented like measurements.
            io = evrows.get("IO:DataFileRead", {})
            for col in ("avg_us", "p50_us", "p95_us", "p99_us", "max_us"):
                t.check(io.get(col, "missing") is None,
                        f"sampled row gates latency column {col} (null)")

            qr = srv.query("top_queries", from_=frm, to_=to)
            t.check_eq(qr.get("fidelity"), "sampled",
                       "top_queries fidelity=sampled")
            qrows = {r["query_id"]: r for r in qr["rows"]}
            t.check_approx(qrows.get("100", {}).get("total_ms", -1),
                           expected_ms, 0.001, "top_queries qid 100 total")

            aas = srv.query("aas", from_=frm, to_=to, buckets=10)
            t.check_eq(aas.get("fidelity"), "sampled", "aas fidelity=sampled")
            active_ms = sum(
                v * aas["bucket_ns"] / 1e6
                for b in aas["buckets"]
                for k, v in b.items()
                if k != "t" and isinstance(v, (int, float)))
            t.check_approx(active_ms, expected_ms, 0.02,
                           "aas integrates to the sampled DB time")

            sess = srv.query("top_sessions", from_=frm, to_=to)
            t.check_eq(sess.get("fidelity"), "sampled",
                       "top_sessions fidelity=sampled")
            t.check(any(r["pid"] == 1000 for r in sess["rows"]),
                    "top_sessions shows the sampled session")
    finally:
        cleanup_traces(trace_dir)


def build_full_300s():
    """--mode full style trace: exact transitions only, spanning 300 s."""
    events = []
    ts = BASE
    for i in range(300):
        ts += S
        events.append({"pid": 1000, "ts": ts, "dur": S,
                       "old": IO_DATA_FILE_READ if i % 2 else CPU,
                       "new": CPU, "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }, BASE, ts


def test_full_mode_reads_raw(t):
    print("\n### 2. Exact-covered long window reads raw and enforces the event limit ###")
    scenario, lo, hi = build_full_300s()
    trace_dir = generate_traces(scenario)
    try:
        frm, to = lo - 10 * S, hi + 10 * S
        commands = ("aas", "time_model", "top_events", "top_sessions", "heatmap")
        with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
            for cmd in commands:
                refused = srv.query(cmd, from_=frm, to_=to, buckets=10)
                t.check_eq(refused.get("code"), "window_too_large",
                           cmd + ": current summaries cannot bypass event limit")
                t.check_eq(refused.get("max_events"), 1,
                           cmd + ": one-event oracle is active")
        with ServerHarness(trace_dir) as srv:
            for cmd in commands:
                answer = srv.query(cmd, from_=frm, to_=to, buckets=10)
                t.check("error" not in answer, cmd + ": normal budget answers")
                t.check_eq(answer.get("fidelity"), "exact",
                           cmd + ": long raw window retains exact fidelity")
            tm = srv.query("time_model", from_=frm, to_=to)
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_eq(tm.get("fidelity"), "exact",
                       "fully exact-covered window labeled exact")
            t.check_approx(rows.get("DB Time", {}).get("ms", -1),
                           300_000.0, 0.001, "DB Time = 300s from raw events")

            # A window with NO data must be labeled none — never exact.
            empty = srv.query("time_model",
                              from_=hi + 1000 * S, to_=hi + 1200 * S)
            t.check_eq(empty.get("fidelity"), "none",
                       "empty >=120s window labeled 'none', not 'exact'")
            erows = {r["name"]: r for r in empty["rows"]}
            t.check_approx(erows.get("DB Time", {}).get("ms", -1), 0.0, 0.001,
                           "empty window DB Time = 0")
    finally:
        cleanup_traces(trace_dir)


def test_io_worker_availability(t):
    print("\n### 4. Long and short raw windows retain measured IO utilization ###")
    scenario, lo, hi = build_full_300s()
    trace_dir = generate_traces(scenario)
    try:
        with tempfile.TemporaryDirectory(prefix="pgwt-io-worker-") as logs:
            for label, frm, to in (("long", lo, hi),
                                   ("short", lo + S, lo + 60 * S)):
                err_path = os.path.join(logs, label + ".log")
                with ServerHarness(trace_dir, stderr_path=err_path) as srv:
                    tm = srv.query("time_model", from_=frm, to_=to)
                with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
                    oracle = srv.query("time_model", from_=frm, to_=to)
                stderr = Path(err_path).read_text()
                t.check_eq(classify_route(oracle, stderr, marker_expected=False),
                           ROUTE_RAW, label + ": production raw route proved")
                key = "io_worker_busy_pct"
                t.check(type(tm.get(key)) in (int, float),
                        label + ": raw utilization is a measured number")
                for suffix in ("_unavailable", "_unavailable_reason"):
                    t.check(key + suffix not in tm, label + ": raw omits " + key + suffix)
                t.check(isinstance(tm.get("categories"), list),
                        label + ": raw retains category rows")
    finally:
        cleanup_traces(trace_dir)


LEGACY_V9_SHA256 = (
    "23cb7e097d4a0fe2aafd74de05b44549e742718b070bfa212f7561d662443418")


# Finalized, unmodified v10 output from the real writer at
# 6c0b78692dbbdece8472f171c3b6c9a8f56738de (7d219c43^), NOT v11 bytes
# relabeled as v10. To reproduce: compile that revision's summary_writer.c
# AND summary_writer.h with -DPGWT_SERVER -Isrc; link gen_test_traces.c as
# described in fixtures/summary_v9_wait_spin/README.md, substituting this
# writer object. Generate v9_wait_spin_scenario() and keep current.summary.
# zlib/base64 only packs the frozen file into this test; the hash pins the
# original 24,212 bytes, including the old per-event layout and footer/index.
LEGACY_V10_SHA256 = (
    "a4f32d85d55e371b3c67fa088c1c2cefe7ca8bb32261ef1f7364187688c7f79c")
LEGACY_V10_ZLIB_BASE64 = (
    "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"
)


def v9_wait_spin_scenario():
    """The frozen pre-#766 writer's input; see the fixture README."""
    events = []
    for sec in range(130):
        start = BASE + sec * S
        for offset, old, dur, cpu in (
                (100, 0xFFFFFFF6, 0, 2**64 - 1),
                (150, CPU, 50_000_000, 2**64 - 1),
                (160, 0x03000000, 10_000_000, 2_000_000),
                (161, 0xFFFFFFF7, 0, 2**64 - 1)):
            events.append({"pid": 41001, "ts": start + offset * 1_000_000,
                           "old": old, "new": old if not dur else IO_DATA_FILE_READ,
                           "dur": dur, "cpu": cpu, "qid": 100})
    return {"cpu_measured": 1, "events": events,
            "backends": [{"pid": 41001, "type": "client"}]}


def test_retired_summary_cpu_split(t):
    print("\n### 3. Current, v9/v10 and mixed files all leave requests on raw ###")
    legacy = os.path.join(os.path.dirname(__file__), "fixtures",
                          "summary_v9_wait_spin", "current.summary")
    # Frozen bytes from the old writer, not an invented version number.
    with open(legacy, "rb") as f:
        raw = f.read()
    t.check_eq(struct.unpack("<IH", raw[:6])[1], 9,
               "retired fixture really is v9")
    # A version byte alone cannot tell the pre-#766 writer's output from a
    # current-writer file patched to 9; the pinned hash (also in the fixture
    # README) is what keeps this evidence from being silently regenerated.
    t.check_eq(hashlib.sha256(raw).hexdigest(), LEGACY_V9_SHA256,
               "retired fixture is the frozen pre-#766 writer's bytes")
    v10 = zlib.decompress(base64.b64decode(LEGACY_V10_ZLIB_BASE64))
    t.check_eq(struct.unpack("<IH", v10[:6])[1], 10,
               "retired fixture really is v10")
    t.check_eq(hashlib.sha256(v10).hexdigest(), LEGACY_V10_SHA256,
               "retired fixture is the frozen pre-#818 writer's bytes")
    retired = {9: raw, 10: v10}
    for version in retired:
        t.check(PGWT_SUMMARY_VERSION > version,
                f"v{version} is retired, not the current-writer control")
    for arm, version, mixed in (
            ("current", None, False),
            ("v9", 9, False), ("mixed_v9", 9, True),
            ("v10", 10, False), ("mixed_v10", 10, True)):
        trace_dir = generate_traces(v9_wait_spin_scenario())
        try:
            current = os.path.join(trace_dir, "current.summary")
            with open(current, "rb") as f:
                t.check_eq(struct.unpack("<IH", f.read(6))[1],
                           PGWT_SUMMARY_VERSION,
                           arm + f": fresh writer stamps v{PGWT_SUMMARY_VERSION}")
            if mixed:
                shutil.copyfile(current, os.path.join(
                    trace_dir, "1970-01-01_02.summary.lz4"))
            if version is not None:
                with open(current, "wb") as f:
                    f.write(retired[version])
                # The frozen file is finalized and has its own footer/index.
                os.unlink(current + ".meta")
            with tempfile.TemporaryDirectory(prefix="pgwt-retired-summary-refusal-") as logs:
                err_path = os.path.join(logs, "server.log")
                with ServerHarness(trace_dir, stderr_path=err_path) as srv:
                    aas = srv.query("aas", from_=BASE, to_=BASE + 130 * S,
                                    buckets=130)
                    tm = srv.query("time_model", from_=BASE, to_=BASE + 130 * S)
                    ts = srv.query("top_sessions", from_=BASE,
                                   to_=BASE + 130 * S)
                with open(err_path) as f:
                    stderr = f.read()
                print(arm + " stderr:\n" + stderr)
                print(arm + " time_model: " + json.dumps({
                    "rows": tm.get("rows"),
                    "has_measured_cpu": tm.get("has_measured_cpu")}))
                rows = {row["name"]: row for row in tm["rows"]}
                t.check("CPU*" in rows and "CPU (running)" not in rows and
                        "CPU (waiting for a core)" not in rows,
                        arm + ": response says CPU*, with no invented split")
                t.check_eq(rows.get("CPU*", {}).get("ms"), 6500,
                           arm + ": all 130 seconds of raw history survive")
                t.check_eq(tm.get("wait_gap_cpu_ms"), 260,
                           arm + ": measured wait spin survives")
                t.check_eq(tm.get("has_measured_cpu"), False,
                           arm + ": UNKNOWN CPU gaps stay unmeasured")
                t.check_eq(ts.get("has_measured_cpu"), False,
                           arm + ": top_sessions does not claim measured CPU")
                t.check_eq(tm.get("db_time_ms"), 7800,
                           arm + ": complete DB Time")
                t.check_eq(len(aas.get("buckets", [])), 130,
                           arm + ": aas answered the requested window")
                # 'cat' is emitted only by handle_aas's raw branch. AAS uses
                # class keys; CPU* is a Time Model row, checked above.
                t.check(bool(aas.get("buckets")) and
                        all("cat" in b for b in aas["buckets"]),
                        arm + ": aas took the raw production route")
                with ServerHarness(trace_dir, env={"PGWT_LOAD_MAX_EVENTS": 1}) as srv:
                    for cmd in ("aas", "time_model", "top_sessions"):
                        refused = srv.query(cmd, from_=BASE, to_=BASE + 130 * S)
                        t.check_eq(refused.get("code"), "window_too_large",
                                   arm + ": " + cmd + " refuses instead of reading summaries")

        finally:
            cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_summary_honesty")
    print(f"=== {t.name} ===")
    test_tiered_15min(t)
    test_full_mode_reads_raw(t)
    test_retired_summary_cpu_split(t)
    test_io_worker_availability(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
