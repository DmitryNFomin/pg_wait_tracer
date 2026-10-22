#!/usr/bin/env python3
"""issue410-measurement/probe.py — #410 bucket-equalization measurement.

NOT a test, NOT a gate, and NOT registered with any runner (see
tests/runner_registration_exemptions.list if it ever needs to be). It is a
re-runnable RECORD of the diagnosis done for #410 on branch fix/answer-contracts:
the plan asked "run both paths on one trace with buckets = range-in-seconds; if
they still diverge, report it and stop" rather than ship a fix. This script is
that run, saved so the numbers in the PR/report are reproducible instead of
living only in prose.

It reuses tests/test_summary_raw_parity.py's exact fixture (build_capture())
and its raw-reference method: copy the generated trace dir, delete the copy's
summary files so pgwt-server's own supported fallback answers the copy from
raw events (no second binary, no env-var override — see that file's header
for why). The untouched original answers from the summary path whenever the
window is >= 120 s and the generated summary covers it.

For each of the five parity windows, it requests `aas` with buckets=10 (the
value tests/test_summary_raw_parity.py's ask() uses, and the value the
checked-in tests/summary_raw_parity_expected.json pins were recorded at) and
again with buckets=<window length in whole seconds> (one bucket per second,
matching the summary path's native per-second granularity), and prints
raw.max_aas / summary.max_aas / % divergence for each.

Needs pgwt-server + tests/gen_test_traces built (e.g. `demos/build-server-local.sh`
on macOS, or a normal Linux `make` in the repo root + `make -C tests`).

Run from anywhere:
    python3 tests/results/issue410-measurement/probe.py
"""
import json
import os
import sys
import glob
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, TESTS_DIR)

from server_harness import ServerHarness, generate_traces, cleanup_traces  # noqa: E402
import test_summary_raw_parity as T  # noqa: E402

WINDOWS_TO_MEASURE = (
    "fractional_bounds", "interior_160s", "shift_1s",
    "to_capture_close", "straddle_130s",
)


def measure():
    sum_dir = generate_traces(T.build_capture())
    raw_dir = sum_dir + "_rawref"
    shutil.copytree(sum_dir, raw_dir)
    removed = glob.glob(os.path.join(raw_dir, "*.summary*"))
    if not removed:
        raise RuntimeError("no summary file to remove — fixture changed? "
                           "cannot produce a raw reference")
    for f in removed:
        os.remove(f)

    rows = []
    try:
        for name in WINDOWS_TO_MEASURE:
            frm, to = T.WINDOWS[name]
            range_s = int((to - frm) // T.S)
            for buckets in (10, range_s):
                with ServerHarness(raw_dir) as srv:
                    r = srv.query("aas", from_=frm, to_=to, buckets=buckets,
                                 limit=100)
                with ServerHarness(sum_dir) as srv:
                    s = srv.query("aas", from_=frm, to_=to, buckets=buckets,
                                 limit=100)
                r_max = r.get("max_aas")
                s_max = s.get("max_aas")
                pct = (s_max - r_max) / r_max * 100 if r_max else None
                rows.append({
                    "window": name,
                    "range_s": range_s,
                    "buckets": buckets,
                    "bucket_ns": r.get("bucket_ns"),
                    "raw_max_aas": r_max,
                    "summary_max_aas": s_max,
                    "diff_pct": round(pct, 1) if pct is not None else None,
                })
    finally:
        cleanup_traces(sum_dir)
        cleanup_traces(raw_dir)
    return rows


def main():
    rows = measure()
    for row in rows:
        print("  window=%-18s buckets=%-4d bucket_ns=%-12s "
              "raw.max_aas=%-10s summary.max_aas=%-10s diff%%=%s"
              % (row["window"], row["buckets"], row["bucket_ns"],
                 row["raw_max_aas"], row["summary_max_aas"], row["diff_pct"]))
    out_path = os.path.join(HERE, "results.json")
    with open(out_path, "w") as f:
        json.dump(rows, f, indent=2)
    print("\nwrote", out_path)


if __name__ == "__main__":
    main()
