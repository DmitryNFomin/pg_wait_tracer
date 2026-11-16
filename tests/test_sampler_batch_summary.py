#!/usr/bin/env python3
"""Mac unit test of the status-batch live test's pure parts (Refs #866).

The live test (tests/test_sampler_batch_live.py) counts syscalls from the
summary `perf trace -S` prints and judges the counts with `assess_counts`.
Both are pure, so they are tested here on outputs recorded on the real gate
images (Ubuntu 24.04 / perf 6.8.12 and EL8 4.18 / perf 4.18): for each, the
candidate must assess GREEN and the recorded output of a tracer built from
master must assess RED at the per-backend file I/O assertion. A gate that
cannot go red is not a gate, so the master recordings are the point.

Fixtures: tests/fixtures/sampler_batch/<image>-pg<N>-<arm>.perf.txt (head of
the `perf trace -S` file, with its unresolved `filename: 0x...` events, plus
the whole summary) and <same>.json (the live test's own summary.json, which
carries the ticks/failures/idle inputs the assessment was run with).
"""
import json
from collections import Counter
from pathlib import Path
import sys
import tempfile
import time
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sampler_batch_perf import (FILE_IO_PER_TICK_BOUND, assess_counts, file_io_reds,  # noqa: E402
                                parse_summary, parse_vm_calls, parse_vm_trace,
                                assess_pg13_vm_calls, pg13_vm_diagnostics,
                                assess_pg13_timing, require_pg13_negative_control)

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "sampler_batch"
CLEAN = {"open": 0, "openat": 6 * 210, "pread64": 2 * 210, "close": 7 * 210,
         "process_vm_readv": 3 * 210}


def pg13_timing_inputs(ticks):
    """Hand-placed E/sample witnesses on a supplied (possibly recorded) cadence.

    These are synthetic SQL/sample witnesses, never reconstructed eligibility
    for an old live run: old recordings have backend_start, not statement E.
    """
    offset = 1_790_000_000_000_000_000
    cohort = [{"pid": 1000 + i, "statement_start_ns": ticks[1] + offset,
               "backend_id": i + 1} for i in range(200)]
    samples = [{"pid": row["pid"], "timestamp_ns": ticks[1] + offset + 100,
                "monotonic_ns": ticks[1] + 100, "query_id": 42} for row in cohort]
    alignment = {"perf_clock": "CLOCK_MONOTONIC", "probes": [
        {"mono_before_ns": t, "wall_ns": t + offset, "mono_after_ns": t}
        for t in (ticks[0] - 1000, ticks[-1] + 1000)]}
    return cohort, samples, alignment


def pg13_observed_tick_cases():
    # Include a 31 ms gap: a nominal 20 ms deadline would wrongly fail here.
    cadences = [("synthetic-jitter", [1_000_000_000, 1_010_000_000,
                                      1_041_000_000, 1_051_000_000])]
    for name, event in (("el8-4.18-file.script.txt", "pgwt866_6907_7473:tick"),
                        ("ubuntu-6.8-ticks.script.txt", "pgwt866_dbg:tick")):
        text = (FIXTURES / "pg13_vm" / name).read_text()
        if name.startswith("ubuntu"):
            # This fixture contains tick lines only. Supply recorded readv
            # pairs for the parser's nonempty-call guard; only Ubuntu's exact
            # event name contributes ticks to the cadence under test.
            text += (FIXTURES / "pg13_vm" / "el8-4.18-file.script.txt").read_text()
        parsed = parse_vm_trace(text, tick_event=event)
        cadences.append((name, [ts for _, ts in parsed["ticks"]]))
    for label, ticks in cadences:
        cohort, samples, alignment = pg13_timing_inputs(ticks)
        green = assess_pg13_timing(cohort, reversed(samples), ticks, alignment, 0)
        assert not green["timing_errors"] and green["attributed_sessions"] == 200, green
        assert set(green["eligible_tick"].values()) == {1}
        assert set(green["first_attribution_tick"].values()) == {1}
        print(f"pg13_at_T_passes [{label}]: PASS")
        # E strictly between ticks still rounds UP to T, however long the gap.
        between = [dict(row, statement_start_ns=row["statement_start_ns"] + 1) for row in cohort]
        at_next = [dict(row, monotonic_ns=ticks[2] + 100) for row in samples]
        delayed_e = assess_pg13_timing(between, at_next, ticks, alignment, 0)
        assert not delayed_e["timing_errors"] and set(delayed_e["eligible_tick"].values()) == {2}
        # E can fall between the marker and the sampler's timestamp read.
        # Those observations still belong to the tick that STARTED before E.
        pre_e_tick = assess_pg13_timing(between, samples, ticks, alignment, 200)
        assert not pre_e_tick["timing_errors"] and pre_e_tick["cohort_pre_e_ticks"] == 200
        # A nonzero sample even before E satisfies 'at T or earlier'.
        early = [dict(samples[0], monotonic_ns=ticks[0] + 100)] + samples[1:]
        assert not assess_pg13_timing(cohort, early, ticks, alignment, 1)["timing_errors"]
        late = samples[:-1] + [dict(samples[-1], monotonic_ns=ticks[2] + 100)]
        red = assess_pg13_timing(cohort, late, ticks, alignment, 0)
        assert red["late_attribution_ticks"] == {1199: 1}
        assert "first attribution late:" in require_pg13_negative_control(red)
        print(f"pg13_at_T_plus_1_RED [{label}]: PASS")
        # Query-id zero or a different pid cannot satisfy missing attribution.
        missing = samples[:-1] + [dict(samples[-1], query_id=0), dict(samples[-1], pid=999)]
        red = assess_pg13_timing(cohort, missing, ticks, alignment, 0)
        assert red["missing_attribution_pids"] == [1199]
        assert "first attribution missing:" in require_pg13_negative_control(red)
        all_missing = assess_pg13_timing(cohort, [], ticks, alignment, 0)
        assert all_missing["attributed_sessions"] == 0
        assert "first attribution missing:" in require_pg13_negative_control(all_missing)
        print(f"pg13_missing_attribution_RED [{label}]: PASS")
        for clock in (None, {}, dict(alignment, perf_clock="unknown"),
                      dict(alignment, probes=alignment["probes"][:1])):
            red = assess_pg13_timing(cohort, samples, ticks, clock, 0)
            assert red["evidence_errors"] and red["timing_errors"]
            try:
                require_pg13_negative_control(red)
            except AssertionError as error:
                assert "invalid timing evidence" in str(error)
            else:
                raise AssertionError("clock error substituted for a negative-control attribution RED")
        # Bracket uncertainty straddles E exactly at a tick: neither endpoint
        # may be chosen to make the result pass.
        probes = [dict(p, mono_before_ns=p["mono_before_ns"] - 1,
                       mono_after_ns=p["mono_after_ns"] + 1) for p in alignment["probes"]]
        red = assess_pg13_timing(cohort, samples, ticks, dict(alignment, probes=probes), 0)
        assert "ambiguous at statement start" in red["evidence_errors"][0]
        print(f"pg13_alignment_unavailable_or_ambiguous_RED [{label}]: PASS")
        # Two copies of the same pre-E pid/tick are ONE allowed failure;
        # an unattributed pre-E point stays counted as promised.
        pre_e = dict(samples[0], query_id=0, monotonic_ns=ticks[0] + 100)
        with_pre_e = [pre_e, pre_e, *samples]
        at_bound = assess_pg13_timing(cohort, with_pre_e, ticks, alignment, 1)
        assert not at_bound["timing_errors"] and at_bound["cohort_pre_e_ticks"] == 1
        red = assess_pg13_timing(cohort, with_pre_e, ticks, alignment, 2)
        assert red["churn_read_failures"] == 2 and red["cohort_pre_e_ticks"] == 1
        assert red["churn_failure_errors"] == ["churn read failures=2 > cohort pre-E ticks=1"]
        try:
            require_pg13_negative_control(red)
        except AssertionError as error:
            assert "did not redden first attribution" in str(error)
        else:
            raise AssertionError("failure counter substituted for a negative-control attribution RED")
        print(f"pg13_churn_failures_above_pre_E_ticks_RED [{label}]: PASS")
        # First observation wins; changing the wall anchor cannot hide T+1.
        duplicates = samples + [dict(samples[0], monotonic_ns=ticks[2] + 100)]
        assert assess_pg13_timing(cohort, duplicates, ticks, alignment, 0) == green
        late = samples[:-1] + [dict(samples[-1], monotonic_ns=ticks[2] + 100,
                                   timestamp_ns=samples[-1]["timestamp_ns"] - 100000000)]
        assert assess_pg13_timing(cohort, late, ticks, alignment, 0)["late_attribution_ticks"] == {1199: 1}
        for invalid in ([dict(samples[0], monotonic_ns=ticks[-1])],
                        [{k: v for k, v in samples[0].items() if k != "monotonic_ns"}]):
            assert assess_pg13_timing(cohort, invalid, ticks, alignment, 0)["evidence_errors"]


def pg13_statement_and_clock_cases():
    import test_sampler_batch_live as live
    connections = mock.Mock()
    connections.query.return_value = [["123", "1790000000000123000", "2", ""]]
    connections.lib.PQbackendPID.return_value = 123
    assert live.churn_statement(connections, 7) == {
        "pid": 123, "statement_start_ns": 1790000000000123000, "backend_id": 2}
    connections.query.assert_called_once_with(7, live.CHURN_SQL)
    assert "statement_timestamp()" in live.CHURN_SQL and "pg_sleep(0.05)" in live.CHURN_SQL
    assert "backend_start" not in live.CHURN_SQL and ";" not in live.CHURN_SQL
    with mock.patch.object(live.time, "clock_gettime_ns", side_effect=[10, 1000, 12]) as clock:
        assert live.clock_probe(1) == {"mono_before_ns": 10, "wall_ns": 1000, "mono_after_ns": 12}
        assert clock.call_args_list == [mock.call(live.time.CLOCK_MONOTONIC),
                                       mock.call(live.time.CLOCK_REALTIME),
                                       mock.call(live.time.CLOCK_MONOTONIC)]
    # A fixed number of brackets; the narrowest valid one is kept (a
    # preempted 51 us bracket must not widen the run's offset envelope).
    reads = [10, 1000, 60, 70, 1062, 73, 80, 1071, 84, 90, 1081, 95, 100, 1090, 140]
    with mock.patch.object(live.time, "clock_gettime_ns", side_effect=reads) as clock:
        assert live.clock_probe() == {"mono_before_ns": 70, "wall_ns": 1062, "mono_after_ns": 73}
        assert clock.call_count == 3 * live.PG13_CLOCK_READS == 15
    print("pg13_one_statement_identity_sleep_and_independent_clock_bracket: PASS")


def expect_raises(label, text, extra=()):
    try:
        parse_summary(text, extra)
    except AssertionError:
        return
    raise AssertionError(f"{label}: incomplete perf evidence accepted")


def summary(*threads):
    """A perf summary as perf prints it: one table per traced thread."""
    text = "\n Summary of events:\n"
    for tid, rows in threads:
        text += f"\n pg_wait_tracer ({tid}), {sum(r[1] for r in rows)} events, 100.0%\n\n"
        text += "   syscall            calls  errors  total       min       avg       max       stddev\n"
        text += "                                     (msec)    (msec)    (msec)    (msec)        (%)\n"
        text += "   --------------- --------  ------ -------- --------- --------- ---------     ------\n"
        for name, calls in rows:
            text += f"   {name:<15} {calls:8d}       0    1.000     0.001     0.002     0.010      1.00%\n"
    return text


def recorded_cases():
    metas = sorted(FIXTURES.glob("*.json"))
    assert metas, f"no recorded fixtures in {FIXTURES}"
    seen = set()
    for meta_path in metas:
        meta = json.loads(meta_path.read_text())
        perf = meta_path.with_suffix(".perf.txt")
        assert perf.exists(), perf
        text = perf.read_text()
        # perf could not resolve path names on any recorded image.
        assert "filename: 0x" in text, f"{perf.name}: fixture is not the unresolved-path form"
        counts = parse_summary(text)
        assert {k: counts[k] for k in meta["calls"] if k != "threads"} == \
               {k: v for k, v in meta["calls"].items() if k != "threads"}, (perf.name, counts, meta["calls"])
        result = assess_counts(counts, meta["major"], 504, meta["ticks"], meta["ticks_spanning"],
                               meta["read_failures"], meta["idle_cohort_samples_lower_bound"])
        assert result["errors"] == meta["errors"], (perf.name, result["errors"], meta["errors"])
        arm = meta["label"]
        if arm == "candidate":
            assert not result["errors"], (perf.name, result["errors"])
            assert max(result["per_tick"][k] for k in ("openat", "pread64", "close")) \
                < FILE_IO_PER_TICK_BOUND / 4, (perf.name, "candidate not far below the bound")
        elif arm == "master-negative":
            reds = file_io_reds(result["errors"])
            assert {e.split("/tick")[0] for e in reds} == {"openat", "pread64", "close"}, (perf.name, reds)
            assert min(result["per_tick"][k] for k in ("openat", "pread64", "close")) \
                > 4 * FILE_IO_PER_TICK_BOUND, (perf.name, "master not far above the bound")
            assert any(e.startswith("vm/tick=") and " < " in e for e in result["errors"]), perf.name
        elif arm == "status-disabled-negative":
            assert any(e.startswith("vm/tick=") for e in result["errors"]), perf.name
            assert any("idle attributed samples=0" in e for e in result["errors"]), perf.name
        else:
            raise AssertionError(f"{perf.name}: unknown arm {arm}")
        seen.add((perf.name.split("-pg")[0], arm))
    images = {image for image, _ in seen}
    for image in images:
        assert (image, "candidate") in seen and (image, "master-negative") in seen, \
            f"{image}: needs a recorded candidate and a recorded master that must assess RED"
    assert {"ubuntu", "el8"} <= images, f"recordings from both images required, have {sorted(images)}"
    print(f"recorded outputs: {len(metas)} files from {sorted(images)}; candidates GREEN, master RED "
          "at openat, pread64 and close per tick")


def bound_cases():
    """Every bound at its edge, and one step past it, for each qualified version."""
    for major, population, minimum, bound in ((13, 504, 3, 5), (17, 504, 3, 5), (18, 504, 3, 5)):
        ticks = 200
        counts = dict(CLEAN)
        counts["process_vm_readv"] = minimum * ticks
        assert not assess_counts(counts, major, population, ticks, ticks, 0, 500)["errors"]
        counts["process_vm_readv"] = minimum * ticks - 1
        assert any(" < " in e for e in assess_counts(counts, major, population, ticks, ticks, 0, 500)["errors"])
        counts["process_vm_readv"] = bound * ticks
        assert not assess_counts(counts, major, population, ticks, ticks, 0, 500)["errors"]
        counts["process_vm_readv"] = bound * ticks + 1
        assert any(" > " in e for e in assess_counts(counts, major, population, ticks, ticks, 0, 500)["errors"])
    # More targets need more row calls: 4 ranges per row on PG13, 3 elsewhere.
    for major, population, minimum in ((13, 1024, 5), (17, 1024, 4)):
        counts = dict(CLEAN, process_vm_readv=minimum * 200)
        assert assess_counts(counts, major, population, 200, 200, 0, 500)["vm_calls_per_tick_minimum"] == minimum
    for name, key in (("openat", "openat"), ("openat", "open"), ("pread64", "pread64"), ("close", "close")):
        counts = dict(CLEAN, openat=0, open=0, pread64=0, close=0)
        counts[key] = FILE_IO_PER_TICK_BOUND * 200
        assert not assess_counts(counts, 18, 504, 200, 200, 0, 500)["errors"], key
        counts[key] += 1
        errors = assess_counts(counts, 18, 504, 200, 200, 0, 500)["errors"]
        assert len(file_io_reds(errors)) == 1 and file_io_reds(errors)[0].startswith(name + "/tick="), (key, errors)
    # Floors that must go red: no idle attribution, structurally unread rows.
    assert any("idle attributed samples=0" in e for e in assess_counts(CLEAN, 18, 504, 200, 200, 0, 0)["errors"])
    assert not assess_counts(CLEAN, 18, 504, 200, 200, 100, 500)["errors"]  # 0.000992 per target-tick
    assert any("read-failure" in e for e in assess_counts(CLEAN, 18, 504, 200, 200, 102, 500)["errors"])
    # Window misalignment never reddens a bound by itself: upper bounds divide
    # by the larger tick count, the lower bound by the smaller.
    counts = dict(CLEAN, process_vm_readv=3 * 224)  # exactly three per tick, perf saw 224 ticks
    assert not assess_counts(counts, 18, 504, 210, 224, 0, 500)["errors"]
    counts = dict(CLEAN, process_vm_readv=3 * 210 - 1)  # fewer than three per certain tick
    assert any(" < " in e for e in assess_counts(counts, 18, 504, 210, 224, 0, 500)["errors"])
    counts = dict(CLEAN, process_vm_readv=int(3.2 * 210))  # 3.2 per tick seen inside 210 ticks
    assert not assess_counts(counts, 18, 504, 210, 224, 0, 500)["errors"]
    counts = dict(CLEAN, process_vm_readv=5 * 224)  # exactly the bound over the spanning window
    assert not assess_counts(counts, 18, 504, 210, 224, 0, 500)["errors"]
    counts = dict(CLEAN, process_vm_readv=5 * 224 + 1)
    assert any(" > " in e for e in assess_counts(counts, 18, 504, 210, 224, 0, 500)["errors"])
    try:
        assess_counts(CLEAN, 16, 504, 200, 200, 0, 500)
    except ValueError as error:
        assert "qualification matrix" in str(error)
    else:
        raise AssertionError("PG16 was qualified")
    print("bounds: process_vm_readv floor/ceiling, file I/O ceiling, idle and failure floors PASS at the edge")


def parser_cases():
    one = summary((7, [("process_vm_readv", 660), ("openat", 1300), ("close", 1400), ("pread64", 420)]))
    counts = parse_summary(one)
    assert (counts["process_vm_readv"], counts["openat"], counts["close"], counts["pread64"],
            counts["open"], counts["threads"]) == (660, 1300, 1400, 420, 0, 1)
    # Several traced threads are summed, and an unrelated syscall row is ignored.
    two = summary((7, [("process_vm_readv", 600), ("openat", 10), ("close", 10)]),
                  (8, [("process_vm_readv", 60), ("pread64", 5), ("futex", 99), ("open", 3)]))
    counts = parse_summary(two)
    assert (counts["process_vm_readv"], counts["openat"], counts["pread64"], counts["open"],
            counts["close"], counts["threads"]) == (660, 10, 5, 3, 10, 2)
    # Event lines before the summary are not counted, whatever they show.
    events = ("380670.501 ( 0.075 ms): 8678 openat(dfd: CWD, filename: 0x2c9c7570) = 50\n"
              "380670.583 ( 0.016 ms): 8678 pread64(fd: 50, buf: 0x7fff2c9c7568, count: 8, pos: 14138096) = 8\n")
    assert parse_summary(events + one)["openat"] == 1300
    # A summary with no syscall of ours is zeros, not an error.
    assert parse_summary(summary((7, [("futex", 4)])))["pread64"] == 0
    # Fail closed.
    expect_raises("empty", "")
    expect_raises("events only", events)
    expect_raises("no table", "\n Summary of events:\n\n pg_wait_tracer (7), 3 events, 100.0%\n")
    expect_raises("no thread", "\n Summary of events:\n   syscall            calls  errors  total\n")
    expect_raises("lost in trace", one + "\nLOST 2 events!\n")
    expect_raises("lost in stderr", one, ("Processed 5 events and lost 3 chunks!",))
    expect_raises("broken row", one.replace("1300       0    1.000", "1300       0    nan"))
    print("summary parser: threads summed, events ignored, fail-closed on missing/lost/garbled PASS")


def recorded_vm_script_cases():
    """Ubuntu 24.04 / kernel 6.8 / perf 6.8.12, copied verbatim from the box."""
    text = (FIXTURES / "pg13_vm" / "ubuntu-6.8-perf-6.8.12.script.txt").read_text()
    expected = dict(pid=1403249, liovcnt=0, riovcnt=0, tid=1403249,
                    timestamp_ns=4_399_374_662_139_000, ret=0)
    assert parse_vm_calls(text) == [expected]
    assert parse_vm_calls("\n".join("  " + line for line in text.splitlines())) == [expected]
    # Synthetic returns retain the recorded bare-value format, including the
    # unsigned representation of a signed 64-bit errno.
    for payload, value in (("0xfffffffffffffff2", -14), ("0xffffffffffffffff", -1),
                           ("0x8000000000000000", -(1 << 63)),
                           ("0x7fffffffffffffff", (1 << 63) - 1), ("48", 48), ("-14", -14)):
        assert parse_vm_calls(text.replace(": 0x0\n", f": {payload}\n")) == [dict(expected, ret=value)]
    decimal = text.replace("pid: 0x00156971", "pid: 1403249").replace("cnt: 0x00000000", "cnt: 0")
    assert parse_vm_calls(decimal) == [expected]
    invalid = [text.replace(": 0x0\n", f": {payload}\n") for payload in
               ("", "unknown", "0x", "0x0garbage", "0x0 trailing", "ret: 0", "ret=0",
                "0x10000000000000000", "9223372036854775808", "-9223372036854775809")]
    invalid += [text.replace("pid: 0x00156971", f"pid: {payload}")
                for payload in ("0x", "0x00156971garbage", "1403249.5", "-1")]
    for bad in invalid:
        try:
            parse_vm_calls(bad)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"undecodable raw readv accepted: {bad}")
    print(f"recorded perf script: 2 verbatim lines; zero, signed hex/decimal, whitespace PASS; {len(invalid)} malformed cases rejected")


def recorded_file_vm_cases():
    """An unedited contiguous EL8 slice, including 50 complete tick intervals."""
    text = (FIXTURES / "pg13_vm" / "el8-4.18-file.script.txt").read_text()
    event = "pgwt866_6907_7473:tick"
    parsed = parse_vm_trace(text, tick_event=event)
    ticks, calls = parsed["ticks"], parsed["calls"]
    assert len(ticks) == 51 and len(calls) == 150 and not parsed["unmatched_exits"]
    assert ticks[0] == (7473, 172_851_570_484)
    assert ticks[-1] == (7473, 173_351_853_766)
    assert all(row["ret"] is not None and row["tid"] == 7473 for row in calls)
    assert Counter(row["tick_ns"] for row in calls) == Counter({ts: 3 for _, ts in ticks[:-1]})
    discovery = Counter({ts: 0 for _, ts in ticks[:-1]})
    shapes = Counter()
    for row in calls:
        assert row["liovcnt"] == row["riovcnt"]
        shapes[(row["pid"], row["liovcnt"], row["ret"])] += 1
        if row["pid"] == 6925 and row["ret"] <= 12 * row["liovcnt"]:
            discovery[row["tick_ns"]] += 1
    assert shapes == {(6925, 1024, 328704): 50, (6925, 928, 297888): 50,
                      (6928, 492, 1968): 50}
    for _, timestamp in ticks[:-1]:
        assert Counter((row["pid"], row["liovcnt"], row["ret"])
                       for row in calls if row["tick_ns"] == timestamp) == \
               {(6925, 1024, 328704): 1, (6925, 928, 297888): 1, (6928, 492, 1968): 1}
    assert list(discovery.values()) == [0] * 50
    # The failed attachment run never reached churn. It proves the parser and
    # steady-state wait shape, and MUST NOT satisfy the discovery qualification.
    try:
        assess_pg13_vm_calls(text, 6925, wait_pids={6928}, tick_event=event)
    except AssertionError as error:
        assert str(error) == "need pending discovery, status and wait-word calls", str(error)
    else:
        raise AssertionError("steady recording qualified without pending discovery")
    # Ubuntu raw20 contains only tick lines. Feed the disjoint real EL8 readv
    # lines too because parse_vm_trace requires syscall evidence; this checks
    # Ubuntu marker decoding, not a fabricated Ubuntu syscall qualification.
    ubuntu = (FIXTURES / "pg13_vm" / "ubuntu-6.8-ticks.script.txt").read_text()
    parsed = parse_vm_trace(ubuntu + text, tick_event="pgwt866_dbg:tick")
    assert len(parsed["ticks"]) == 20 and len(parsed["calls"]) == 150
    assert parsed["ticks"][0] == (11544, 276_763_187_771)
    assert parsed["ticks"][-1] == (11544, 276_955_095_868)
    assert all(tid == 11544 for tid, _ in parsed["ticks"])
    print("recorded_file_vm_cases: EL8 51 ticks / 150 readv pairs / zero discovery per tick / "
          "50 wait-shaped calls; Ubuntu 20 verbatim tick lines PASS")


def fake_perf_ack(ctl, ack, reply=b"ack\n"):
    """Stand-in perf: read `enable` from the control FIFO, then acknowledge."""
    import os
    import threading
    def run():
        fd = os.open(ctl, os.O_RDONLY | os.O_NONBLOCK)
        data = b""
        try:
            for _ in range(500):
                try:
                    data += os.read(fd, 64)
                except BlockingIOError:
                    pass
                if b"enable\n" in data:
                    break
                time.sleep(.01)
        finally:
            os.close(fd)
        if b"enable\n" in data and reply:
            out = os.open(ack, os.O_WRONLY | os.O_NONBLOCK)
            os.write(out, reply)
            os.close(out)
    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def perf_ack_cases():
    """Attachment is proven by perf's own enable ack, never by elapsed time."""
    import test_sampler_batch_live as live
    for reply, exits, reason in ((b"ack\n", False, None), (b"", False, "did not acknowledge enable"),
                                 (b"", True, "exited 1 before acknowledging enable")):
        with tempfile.TemporaryDirectory(prefix="pgwt866-ack-test-") as tmp:
            trace = Path(tmp)
            proc = mock.Mock(returncode=1 if exits else None)
            proc.poll.side_effect = lambda: proc.returncode
            def spawn(command, **kwargs):
                fake_perf_ack(trace / "perf.ctl", trace / "perf.ack", reply)
                return proc
            with mock.patch.object(live, "spawn", side_effect=spawn):
                try:
                    perf = live.start_pg13_perf(7473, "g:tick", trace, ack_seconds=1)
                except AssertionError as error:
                    assert reason and reason in str(error), (reason, str(error))
                else:
                    assert reason is None and perf is proc
    print("perf_ack_cases: enable acknowledged; no ack and early exit both fail PASS")


def file_capture_cases():
    """The recorder exposes nothing until stopped; decoding must follow wait."""
    import test_sampler_batch_live as live
    text = (FIXTURES / "pg13_vm" / "el8-4.18-file.script.txt").read_text()
    event = "pgwt866_6907_7473:tick"
    for record_error, script_error, decoded, reason in (
            ("", "", text, None),
            ("LOST 1 events!", "", text, "lost events"),
            ("", "LOST 1 events!", text, "lost events"),
            ("", "", text + "PERF_RECORD_LOST id=7 lost=2", "lost events"),
            ("", "", text.replace(event, "other:tick"), "no closed tick interval"),
            ("", "", text.splitlines()[0], "no process_vm_readv")):
        with tempfile.TemporaryDirectory(prefix="pgwt866-file-test-") as tmp:
            trace = Path(tmp)
            order = []
            proc = mock.Mock(returncode=None)
            proc.poll.side_effect = lambda: proc.returncode
            proc.send_signal.side_effect = lambda sig: order.append(("signal", sig))
            def wait(timeout):
                assert order == [("signal", live.signal.SIGINT)]
                order.append("wait")
                (trace / "perf.attach.data").write_bytes(b"recorded after stop")
                proc.returncode = 0
                return 0
            proc.wait.side_effect = wait
            def spawn(command, **kwargs):
                assert command[:2] == ["perf", "record"]
                assert command[command.index("--clockid") + 1] == "mono"
                assert command[command.index("-o") + 1] == str(trace / "perf.attach.data")
                assert command[command.index("-e") + 1] == live.VM_EVENTS + "," + event
                assert command[command.index("-p") + 1] == "7473"
                assert command[command.index("-D") + 1] == "-1"
                assert command[command.index("--control") + 1] == \
                    f"fifo:{trace / 'perf.attach.ctl'},{trace / 'perf.attach.ack'}"
                assert kwargs["stdout"] == live.subprocess.DEVNULL and "stdin" not in kwargs
                kwargs["stderr"].write(record_error)
                fake_perf_ack(trace / "perf.attach.ctl", trace / "perf.attach.ack")
                return proc
            def decode(command, **kwargs):
                assert order == [("signal", live.signal.SIGINT), "wait"]
                assert command == ["perf", "script", "-i", str(trace / "perf.attach.data"),
                                   "--ns", "--show-lost-events", "-F", "tid,time,event,trace"]
                assert (trace / "perf.attach.data").read_bytes() == b"recorded after stop"
                assert kwargs["check"] is True
                order.append("decode")
                kwargs["stdout"].write(decoded)
                kwargs["stderr"].write(script_error)
                return live.subprocess.CompletedProcess(command, 0)
            with mock.patch.object(live, "spawn", side_effect=spawn), \
                 mock.patch.object(live.subprocess, "run", side_effect=decode):
                perf = live.start_pg13_perf(7473, event, trace, "perf.attach")
                assert not (trace / "perf.attach.data").exists()
                try:
                    result, streams = live.decode_pg13_perf(perf, event, trace, "perf.attach")
                except AssertionError as error:
                    assert reason and reason in str(error), (reason, str(error))
                else:
                    assert reason is None and result == text and streams == ("", "")
                assert order[-1] == "decode"
    print("file_capture_cases: regular file, SIGINT/wait before decode, attachment/loss guards PASS")


def pg13_selection_cases():
    """Exercise main's selection boundary without starting a live process."""
    import test_sampler_batch_live as live
    class Selected(Exception):
        pass
    for argv, expected_call, found in ((["--pg13-far-rows"], {"pg_version": 13}, 13),
                                       (["--pg13-far-rows"], {"pg_version": 13}, None),
                                       ([], {}, 18), (["--pg13-far-rows", "--pid", "77"], None, 77)):
        def readlink(path):
            assert path == f"/proc/{found}/exe"
            raise Selected
        with mock.patch.object(live.sys, "argv", ["test_sampler_batch_live.py", *argv]), \
             mock.patch.object(live.sys, "platform", "linux"), \
             mock.patch.object(live.os, "geteuid", return_value=0), \
             mock.patch.object(live, "find_postmaster", return_value=found) as find, \
             mock.patch.object(live.os, "readlink", side_effect=readlink):
            try:
                live.main()
            except Selected:
                assert found
            except AssertionError as error:
                assert found is None and "no PG13 postmaster found" in str(error), str(error)
            else:
                raise AssertionError("selection boundary not exercised")
            if expected_call is None:
                find.assert_not_called()
            else:
                find.assert_called_once_with(**expected_call)
    print("pg13_selection_cases: PG13 filter, missing-PG13 diagnostic, explicit pid and default selection PASS")


def vm_parser_cases():
    """Verbatim historical fragments and hand-built raw tracepoint pairs."""
    for image, count, incomplete, postmaster, waiter in (
            ("ubuntu", 12, 1, 14624, 14626), ("el8", 18, 2, 9190, 9193)):
        text = (FIXTURES / "pg13_vm" / (image + ".perf.txt")).read_text()
        records = parse_vm_calls(text)
        assert len(records) == count and sum(r["ret"] is None for r in records) == incomplete
        assert {r["pid"] for r in records} == {postmaster, waiter}
        # Unresolved libc-only stacks do not affect parsing.
        without_frames = "\n".join(line for line in text.splitlines()
                                   if "process_vm_readv(" in line)
        assert parse_vm_calls(without_frames) == records
        try:
            assess_pg13_vm_calls(text, postmaster, wait_pids={waiter})
        except AssertionError as error:
            assert "unfinished readv" in str(error), str(error)
        else:
            raise AssertionError("historical missing exits qualified as complete evidence")
    # Different timestamps on one tid are DIFFERENT calls, even with identical
    # arguments (Ubuntu); EL8 also has different vectors and multiple entries.
    paired = ("1.000 (         ): 77 process_vm_readv(pid: 42, liovcnt: 1, riovcnt: 1) ...\n"
              "1.000 ( 0.001 ms): 77 ... [continued]: process_vm_readv()) = 12\n")
    assert parse_vm_calls(paired) == [dict(pid=42, liovcnt=1, riovcnt=1,
                                         tid=77, timestamp_ns=1_000_000, ret=12)]

    try:
        parse_vm_calls(paired + summary((77, [("process_vm_readv", 2)])))
    except AssertionError as error:
        assert "census differs" in str(error)
    else:
        raise AssertionError("rendered event/summary census mismatch accepted")

    tick_event = "pgwt866_test:tick"

    def raw(rows, mark_after_wait=True):
        """perf script --ns -F tid,time,event,trace, decimal and hex payloads."""
        text = ""
        previous_wait = False
        for index, (tid, pid, iovs, remote, result) in enumerate(rows):
            if index == 0 or (previous_wait and mark_after_wait):
                text += f"77 1234567.{index * 3:09d}: {tick_event}: (0x1234)\n"
            text += (f"{tid} 1234567.{index * 3 + 1:09d}: syscalls:sys_enter_process_vm_readv: "
                     f"pid: 0x{pid:08x}, lvec: 0x00000000, liovcnt: 0x{iovs:08x}, "
                     f"rvec: 0x00000000, riovcnt: 0x{remote:08x}, flags: 0x00000000\n"
                     f"{tid} 1234567.{index * 3 + 2:09d}:  syscalls:sys_exit_process_vm_readv: "
                     f"0x{result & ((1 << 64) - 1):x}\n")
            previous_wait = pid == 99
        text += f"77 1234567.{len(rows) * 3:09d}: {tick_event}: (0x1234)\n"
        return text

    discover = (77, 42, 4, 4, 48)  # also a possible four-range short status read
    status = (77, 42, 1024, 1024, 328704)
    wait = (77, 99, 500, 500, 2000)
    good = raw([discover, discover, status, wait, status, wait])
    result = assess_pg13_vm_calls(good, 42, wait_pids={99}, ticks_inside=2, ticks_spanning=3,
                                  tick_event=tick_event)
    assert result["max_discovery_calls_per_tick"] == 2
    assert result["calls"]["process_vm_readv"] == 6
    records = parse_vm_calls(good)
    assert records[0]["timestamp_ns"] + 3 == records[1]["timestamp_ns"]
    assert parse_vm_calls("\n".join(reversed(good.splitlines()))) == records
    boundary = ("77 1234566.999999999:  syscalls:sys_exit_process_vm_readv: 0xc\n" + good +
                "77 1234567.999999999: syscalls:sys_enter_process_vm_readv: pid: 42, liovcnt: 1, riovcnt: 1\n")
    bounded = assess_pg13_vm_calls(boundary, 42, wait_pids={99}, tick_event=tick_event)
    assert bounded["boundary_entries"] == bounded["boundary_exits"] == 1
    assert bounded["qualified_ticks"] == 2 and bounded["max_discovery_calls_per_tick"] == 2
    # Interleaved threads pair independently. Same entry time on another tid
    # is not a duplicate, and a signed hexadecimal errno remains negative.
    interleaved = ("7 1.000000001: syscalls:sys_enter_process_vm_readv: pid: 42, liovcnt: 1, riovcnt: 1\n"
                   "8 1.000000001: syscalls:sys_enter_process_vm_readv: pid: 43, liovcnt: 2, riovcnt: 2\n"
                   "8 1.000000002:  syscalls:sys_exit_process_vm_readv: 0x10\n"
                   "7 1.000000003:  syscalls:sys_exit_process_vm_readv: 0xfffffffffffffff2\n")
    assert [r["ret"] for r in parse_vm_calls(interleaved)] == [-14, 16]
    invalid = [
        (raw([discover, discover, discover, status, wait, status, wait]), "calls/tick=3"),
        (raw([status, wait]), "need pending discovery"),
        (raw([(77, 99, 500, 500, 6000), status, wait]), "non-postmaster"),
        (raw([(77, 43, 4, 4, 1284), status, wait]), "non-postmaster"),
        (raw([(77, 42, 4, 3, 1284), status, wait]), "iovec shape"),
        (raw([discover, (77, 42, 5, 5, 1284), wait]), "status batch"),
        (raw([discover, (78, 42, 4, 4, 1284), wait]), "sampler tid"),
        (raw([discover, wait, discover, wait], mark_after_wait=False), "extra wait-shaped"),
        (raw([discover, status, wait]).replace(
             "77 1234567.000000007:", "77 1234567.000000006: pgwt866_test:tick: (0x1234)\n77 1234567.000000007:"),
         "missing wait-word"),
        (good + "LOST 1 events!", "lost events"),
        (good + "PERF_RECORD_LOST id=7 lost=2", "lost events"),
        (good.replace("77 1234567.000000002:  syscalls:sys_exit_process_vm_readv: 0x30\n", ""),
         "overlapping raw"),
    ]
    for text, reason in invalid:
        try:
            assess_pg13_vm_calls(text, 42, wait_pids={99}, tick_event=tick_event)
        except AssertionError as error:
            assert reason in str(error), (reason, str(error))
        else:
            raise AssertionError(f"bad raw evidence accepted: {reason}")
    for kwargs, reason in (({"ticks_inside": 3}, "fewer closed tick markers"),
                           ({"ticks_spanning": 1}, "extra tick markers"),
                           ({"read_faults": 1}, "faults prevent"),
                           ({"tick_event": None}, "independent sampler tick")):
        options = dict(wait_pids={99}, tick_event=tick_event)
        options.update(kwargs)
        try:
            assess_pg13_vm_calls(good, 42, **options)
        except AssertionError as error:
            assert reason in str(error), (reason, str(error))
        else:
            raise AssertionError(f"tick census/refusal accepted: {kwargs}")
    for text, reason in ((interleaved.splitlines()[2], "exit without entry"),
                         (interleaved.splitlines()[0] + "\n" + interleaved.splitlines()[0], "overlapping raw"),
                         (good.replace("riovcnt: 0x00000004", "undecoded: 0x00000004"), "arguments not decoded")):
        try:
            parse_vm_calls(text)
        except AssertionError as error:
            assert reason in str(error), (reason, str(error))
        else:
            raise AssertionError(f"incomplete raw evidence accepted: {reason}")
    assert "evidence_error" in pg13_vm_diagnostics(interleaved.splitlines()[2])
    assert pg13_vm_diagnostics(raw([(77, 1234, 1, 1, 12)]))["calls"]["process_vm_readv"] == 1
    for stream in ("LOST 195 events!", "LOST 217 events!", "LOST 16 events!", "LOST 2 events!"):
        try:
            pg13_vm_diagnostics(good, (stream,))
        except AssertionError as error:
            assert "lost events" in str(error)
        else:
            raise AssertionError("loss in perf stderr accepted")
    print("PG13 readv: Ubuntu/EL8 recorded fragments; raw tid/timestamp pairs, ambiguous discovery budget, loss guards PASS")


def cohort_file_cases():
    """The sample reader gets only the daemon's trace files, never perf text.

    Live on Ubuntu the perf decode wrote perf.trace/perf.attach.trace beside
    current.trace and the binary reader refused them ("bad magic").
    """
    import test_sampler_batch_live as live
    with tempfile.TemporaryDirectory(prefix="pgwt866-cohort-test-") as tmp:
        trace = Path(tmp)
        for name in ("current.trace", "2026-11-16_18.trace.lz4", "2026-11-16_18.1.trace.lz4",
                     "perf.trace", "perf.attach.trace", "perf.script.txt", "perf.attach.script.txt",
                     "perf.data", "2026-11-16_18.summary.lz4"):
            (trace / name).write_bytes(b"x")
        seen = []
        def run(command, **kwargs):
            seen.append(command)
            return live.subprocess.CompletedProcess(command, 0, '{"pid":5,"timestamp_ns":1,"query_id":0}\n',
                                                    "persisted sample census=1\n")
        with mock.patch.object(live.subprocess, "run", side_effect=run):
            samples = live.read_cohort_samples(Path("/reader"), trace, [{"pid": 5}])
        assert samples == [{"pid": 5, "timestamp_ns": 1, "query_id": 0}], samples
        assert seen == [["/reader", str(trace / "churn.pids"), str(trace / "current.trace"),
                         str(trace / "2026-11-16_18.1.trace.lz4"), str(trace / "2026-11-16_18.trace.lz4")]], seen
        for name in ("perf.trace", "perf.attach.trace"):
            (trace / name).unlink()
    print("cohort_file_cases: only current.trace and rotated *.trace.lz4 reach the sample reader PASS")


def postmaster_name_cases():
    """PGDG EL starts PG13 as comm `postmaster` (exe /usr/pgsql-13/bin/postgres);
    its children inherit that comm. Debian/Ubuntu's comm is `postgres`."""
    import testutil
    for comm, exe in (("postmaster", "/usr/pgsql-13/bin/postgres"),
                      ("postgres", "/usr/lib/postgresql/13/bin/postgres")):
        # pid 10 postmaster (parent 1 = systemd), pid 11 its child.
        status = {10: "PPid:\t1\n", 11: "PPid:\t10\n"}
        comms = {1: "systemd", 10: comm}
        def run(command, **kwargs):
            assert command[:2] == ["pgrep", "-x"], command
            out = "10\n11\n" if command[2] == comm else ""
            return testutil.subprocess.CompletedProcess(command, 0 if out else 1, out, "")
        def fake_open(path, *args, **kwargs):
            parts = path.split("/")
            pid = int(parts[2])
            import io
            if parts[3] == "status":
                return io.StringIO(status[pid])
            return io.StringIO(comms[pid] + "\n")
        with mock.patch.object(testutil.subprocess, "run", side_effect=run), \
             mock.patch.object(testutil.os, "readlink", side_effect=lambda path: exe), \
             mock.patch("builtins.open", side_effect=fake_open):
            assert testutil.find_postmaster(pg_version=13) == 10, comm
            assert testutil.find_postmaster(pg_version=16) is None, comm
            assert testutil.find_postmaster() == 10, comm
    print("postmaster_name_cases: PGDG `postmaster` and Debian `postgres` comm both found, children skipped PASS")


def main():
    pg13_observed_tick_cases()
    pg13_statement_and_clock_cases()
    parser_cases()
    bound_cases()
    recorded_cases()
    recorded_vm_script_cases()
    recorded_file_vm_cases()
    file_capture_cases()
    perf_ack_cases()
    pg13_selection_cases()
    cohort_file_cases()
    postmaster_name_cases()
    vm_parser_cases()
    print("sampler batch summary: PASS")


if __name__ == "__main__":
    main()
