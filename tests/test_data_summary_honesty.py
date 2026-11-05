#!/usr/bin/env python3
"""test_data_summary_honesty.py — T1/FID-2: the summary fast-path must be
coverage-aware and the fidelity label must reflect the actual data source.

Before the fix, should_use_summaries() selected the summary path on range
length alone (>= 120 s) and stamped "fidelity":"exact" — but summaries are
fed only by the exact (watchpoint) path. In the DEFAULT tiered mode a
"last 15 minutes" query therefore returned escalation slivers or nothing,
labeled exact: the worst possible violation of "never a silent empty
result" and of the standing rule that "last 15 min must mean NOW".

After the fix:
  * a >= 120 s window whose sampled data is not fully covered by exact data
    falls back to the raw (merge) path → correct ASH-estimated values,
    honest "sampled"/"mixed" label;
  * a >= 120 s window fully covered by exact data still uses the summary
    fast path and is honestly labeled "exact";
  * a window with no data at all is labeled "none" — never "exact".
"""
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE = 10_000_000_000_000
S = 1_000_000_000
PERIOD = 1 * S      # 1 Hz sampling keeps the sample count small


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


def test_full_mode_still_fast(t):
    print("\n### 2. Exact-covered long window keeps the summary path ###")
    scenario, lo, hi = build_full_300s()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            frm, to = lo - 10 * S, hi + 10 * S
            tm = srv.query("time_model", from_=frm, to_=to)
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_eq(tm.get("fidelity"), "exact",
                       "fully exact-covered window labeled exact")
            t.check_approx(rows.get("DB Time", {}).get("ms", -1),
                           300_000.0, 0.001, "DB Time = 300s from summaries")

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


LEGACY_V9_SHA256 = (
    "23cb7e097d4a0fe2aafd74de05b44549e742718b070bfa212f7561d662443418")


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


def test_retired_v9_cpu_split(t):
    print("\n### 3. Real v9 refuses the whole window; v10 stays on summaries ###")
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
    for arm in ("v10", "v9", "mixed"):
        trace_dir = generate_traces(v9_wait_spin_scenario())
        try:
            current = os.path.join(trace_dir, "current.summary")
            with open(current, "rb") as f:
                t.check_eq(struct.unpack("<IH", f.read(6))[1], 10,
                           arm + ": fresh writer stamps v10")
            if arm == "mixed":
                shutil.copyfile(current, os.path.join(
                    trace_dir, "1970-01-01_02.summary.lz4"))
            if arm != "v10":
                shutil.copyfile(legacy, current)
                # The frozen file is finalized and has its own footer/index.
                os.unlink(current + ".meta")
            with tempfile.TemporaryDirectory(prefix="pgwt-v9-refusal-") as logs:
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
                t.check(all(("cat" in b) == (arm != "v10")
                            for b in aas.get("buckets", [])),
                        arm + ": aas took the required production route")
                if arm == "v10":
                    t.check("recomputing from raw" not in stderr,
                            "v10-only control still takes summaries")
                else:
                    marker = ("INFO: 1 of %d summary file(s) covering this "
                              "window are not summary v10 -- recomputing "
                              "from raw events" % (2 if arm == "mixed" else 1))
                    t.check_eq(stderr.count(marker), 2,
                               arm + ": aas AND time_model log the v10 refusal")
        finally:
            cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_summary_honesty")
    print(f"=== {t.name} ===")
    test_tiered_15min(t)
    test_full_mode_still_fast(t)
    test_retired_v9_cpu_split(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
