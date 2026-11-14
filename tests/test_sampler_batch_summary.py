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
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sampler_batch_perf import (FILE_IO_PER_TICK_BOUND, assess_counts, file_io_reds,  # noqa: E402
                                parse_summary)

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "sampler_batch"
CLEAN = {"open": 0, "openat": 6 * 210, "pread64": 2 * 210, "close": 7 * 210,
         "process_vm_readv": 3 * 210}


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


def main():
    parser_cases()
    bound_cases()
    recorded_cases()
    print("sampler batch summary: PASS")


if __name__ == "__main__":
    main()
