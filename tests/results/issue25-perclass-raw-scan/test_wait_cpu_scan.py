#!/usr/bin/env python3
"""Bypass suite for tests/wait_cpu_scan.c.

Showing that the scan can go RED on a planted outlier proves only the
true-positive path, which was never the broken one. Every case below is a way
the scan's DETECTION could be made unreachable — a false negative — and each
asserts the tool REFUSES (exit 2) or reports UNMEASURED (exit 4) instead of
reporting a clean verdict:

  empty input, a missing directory, a truncated/unreadable trace, cpu_ns that
  was never measured, a pure-sleep class that is absent rather than wrong, a
  record census that disagrees with the independent total (a partial range),
  an unparseable (unresolvable) wait-event name, a trace with no wait records
  at all, and the census dependency being unavailable (`pgwt-server --dump`
  exiting 126/127) so the completeness check cannot run.

Run:  python3 tests/results/issue25-perclass-raw-scan/test_wait_cpu_scan.py
Env:  SCAN_BIN / FIXTURE_BIN override the binaries (used to drive mutants of
      the scanner and show each assertion red).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SCAN = os.environ.get("SCAN_BIN", os.path.join(HERE, "bin", "wait_cpu_scan"))
FIXTURE = os.environ.get("FIXTURE_BIN", os.path.join(HERE, "bin", "make_fixture"))
THRESHOLD = "824430"

PGSLEEP = 150994946        # (0x09 << 24) | 2   Timeout:PgSleep
RELATION = 50331648        # (0x03 << 24) | 0   Lock:relation
MARKER = 0xFFFFFFF0        # PGWT_MARKER_EXEC_START
NO_SUCH_CLASS = 0x7F000001  # class 0x7F: no name in any table

failures = []
passes = 0


def check(name, cond, detail=""):
    global passes
    if cond:
        passes += 1
        print(f"  ok   {name}")
    else:
        failures.append(f"{name}: {detail}")
        print(f"  FAIL {name}: {detail}")


def fixture(tmp, spec):
    d = tempfile.mkdtemp(dir=tmp)
    out = subprocess.run([FIXTURE, d, spec], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    return d, int(out.stdout.strip())


def scan(trace_dir, *extra, expect_transitions=None, expect_samples=0,
         pure_sleep="Timeout:PgSleep,Lock:relation"):
    out_json = tempfile.mktemp(suffix=".json")
    cmd = [SCAN, "--trace-dir", trace_dir, "--threshold-ns", THRESHOLD,
           "--pure-sleep", pure_sleep, "--output", out_json]
    if expect_transitions is not None:
        cmd += ["--expect-transitions", str(expect_transitions)]
    if expect_samples is not None:
        cmd += ["--expect-samples", str(expect_samples)]
    cmd += list(extra)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    data = None
    if os.path.exists(out_json):
        with open(out_json) as f:
            try:
                data = json.load(f)
            except json.JSONDecodeError:
                data = None
        os.unlink(out_json)
    return proc.returncode, data


def main():
    tmp = tempfile.mkdtemp(prefix="wait_cpu_scan_tests_")
    try:
        # --- control: the CLEAN verdict has to be reachable at all, else every
        # "not clean" assertion below is vacuous. ------------------------------
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                            f"{RELATION}:4:200000000:25000")
        rc, data = scan(d, expect_transitions=n)
        check("control_clean_is_reachable", rc == 0 and data["verdict"] == "CLEAN",
              f"rc={rc} verdict={data and data['verdict']}")
        check("control_reports_per_event_max",
              any(c["name"] == "Timeout:PgSleep" and c["max_cpu_ns"] == 30000
                  for c in data["classes"]) if data else False,
              "per-class max cpu_ns missing")

        # --- true positive: a planted millisecond-scale sleep CPU ------------
        d, n = fixture(tmp, f"{PGSLEEP}:4:3000000000:30000,"
                            f"{PGSLEEP}:1:3000000000:1200000,"
                            f"{RELATION}:4:200000000:25000")
        rc, data = scan(d, expect_transitions=n)
        ids = [c["event"]["cpu_ns"] for c in data["crossing_events"]] if data else []
        check("crossing_detected", rc == 3 and data["verdict"] == "CROSSING"
              and 1200000 in ids, f"rc={rc} crossings={ids}")
        check("crossing_carries_full_identity",
              bool(data) and data["crossing_events"]
              and data["crossing_events"][0]["event"]["pid"] > 0
              and data["crossing_events"][0]["event"]["duration_ns"] == 3000000000,
              "identity incomplete")

        # --- bypass 1: empty input ------------------------------------------
        empty = tempfile.mkdtemp(dir=tmp)
        rc, data = scan(empty, expect_transitions=0)
        check("empty_dir_refuses", rc == 2 and "no_trace_files" in data["refusals"],
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 2: the input is not there at all -------------------------
        rc, data = scan(os.path.join(tmp, "does-not-exist"), expect_transitions=0)
        check("missing_dir_refuses",
              rc == 2 and any("unreadable" in r for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 3: a truncated trace -------------------------------------
        # Asserted on the SPECIFIC decode guard, not merely on exit 2: the
        # census-mismatch refusal also fires here, so "rc == 2" alone passes
        # even with the decode guard removed (caught by mutant M4).
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                            f"{RELATION}:4:200000000:25000")
        path = os.path.join(d, "current.trace")
        size = os.path.getsize(path)
        with open(path, "r+b") as f:
            f.truncate(size // 2)
        rc, data = scan(d, expect_transitions=n)
        check("truncated_trace_refuses",
              rc == 2 and data["coverage"]["blocks_failed"] > 0
              and any(r.startswith("block_decode_failed") or
                      r.startswith("block_header_unreadable")
                      for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")

        # A trace file whose header is intact but whose body/footer is gone:
        # pgwt_reader_open() itself fails, and that must be counted as a FAILED
        # file rather than treated as a directory with nothing in it.
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                            f"{RELATION}:4:200000000:25000")
        with open(os.path.join(d, "current.trace"), "r+b") as f:
            f.truncate(40)
        rc, data = scan(d, expect_transitions=n)
        check("unreadable_trace_file_refuses",
              rc == 2 and data["coverage"]["files_failed"] == 1
              and any(r.startswith("trace_file_unreadable")
                      for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")

        # A partial decode (fewer records than the block header promises) is a
        # shortfall, never silently smaller coverage.
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000")
        rc, data = scan(d, expect_transitions=n)
        check("record_census_equals_block_headers",
              rc in (0, 3, 4) and data["coverage"]["records_walked"]
              == data["coverage"]["header_records"] == n,
              f"rc={rc} coverage={data and data['coverage']}")

        # --- bypass 4: cpu_ns never measured (legacy capability) -------------
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:unknown,"
                            f"{RELATION}:4:200000000:unknown")
        rc, data = scan(d, expect_transitions=n)
        unm = set(data["unmeasured_pure_sleep_classes"]) if data else set()
        sleep_cls = {c["name"]: c for c in data["classes"]} if data else {}
        check("unknown_cpu_is_unmeasured_not_clean",
              rc == 4 and unm == {"Timeout:PgSleep", "Lock:relation"},
              f"rc={rc} unmeasured={unm}")
        check("unknown_cpu_counted_not_zeroed",
              bool(sleep_cls) and sleep_cls["Timeout:PgSleep"]["cpu_unknown"] == 5
              and sleep_cls["Timeout:PgSleep"]["measured"] == 0,
              f"{sleep_cls.get('Timeout:PgSleep')}")

        # --- bypass 5: the class is ABSENT rather than wrong -----------------
        d, n = fixture(tmp, f"{RELATION}:4:200000000:25000")
        rc, data = scan(d, expect_transitions=n)
        check("absent_class_is_unmeasured_not_clean",
              rc == 4 and data["unmeasured_pure_sleep_classes"] == ["Timeout:PgSleep"],
              f"rc={rc} unmeasured={data and data['unmeasured_pure_sleep_classes']}")

        # --- bypass 6: a PARTIAL range — census below the independent total --
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                            f"{RELATION}:4:200000000:25000")
        rc, data = scan(d, expect_transitions=n + 11)
        check("census_mismatch_refuses",
              rc == 2 and any(r.startswith("transition_census_mismatch")
                              for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")
        rc, data = scan(d, expect_transitions=n, expect_samples=7)
        check("sample_census_mismatch_refuses",
              rc == 2 and any(r.startswith("sample_census_mismatch")
                              for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 7: no independent total supplied at all ------------------
        rc, data = scan(d, expect_transitions=None, expect_samples=None)
        check("no_expected_total_refuses",
              rc == 2 and "coverage_unverified:no_expect_transitions" in data["refusals"],
              f"rc={rc} refusals={data and data['refusals']}")
        rc, data = scan(d, "--no-expect-check", expect_transitions=n)
        check("disabled_coverage_check_refuses",
              rc == 2 and "coverage_unverified:expect_check_disabled" in data["refusals"],
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 8: an unparseable (unresolvable) wait-event name ---------
        # A pure-sleep class whose NAME cannot be resolved looks absent, and
        # absent must not read as clean — even when it carries a crossing.
        d, n = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                            f"{RELATION}:4:200000000:25000,"
                            f"{NO_SUCH_CLASS}:2:3000000000:1500000")
        rc, data = scan(d, expect_transitions=n)
        check("unresolvable_name_refuses",
              rc == 2 and any(r.startswith("unresolved_event_names")
                              for r in data["refusals"]),
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 9: structurally valid trace with NO wait records ---------
        d, n = fixture(tmp, f"{MARKER}:6:1000000:5000")
        rc, data = scan(d, expect_transitions=n)
        check("marker_only_trace_refuses",
              rc == 2 and "no_wait_records" in data["refusals"],
              f"rc={rc} refusals={data and data['refusals']}")

        # --- bypass 10: the census dependency cannot run (126/127) ----------
        # run_evidence.sh must fail, never fall back to a coverage-free scan.
        for code, mode in ((127, "missing"), (126, "nonexec")):
            fake = tempfile.mkdtemp(dir=tmp)
            binsdir = os.path.join(fake, "bin")
            os.makedirs(binsdir)
            shutil.copy(SCAN, os.path.join(binsdir, "wait_cpu_scan"))
            shutil.copy(FIXTURE, os.path.join(binsdir, "make_fixture"))
            if mode == "nonexec":
                with open(os.path.join(binsdir, "pgwt-server"), "w") as f:
                    f.write("#!/bin/sh\nexit 0\n")
                os.chmod(os.path.join(binsdir, "pgwt-server"), 0o644)
            trace, _ = fixture(tmp, f"{PGSLEEP}:5:3000000000:30000,"
                                    f"{RELATION}:4:200000000:25000")
            proc = subprocess.run(
                ["bash", os.path.join(HERE, "run_evidence.sh"), trace,
                 os.path.join(fake, "out")],
                capture_output=True, text=True,
                env={**os.environ, "PGWT_SCAN_BIN_DIR": binsdir,
                     "PGWT_SKIP_BUILD": "1"})
            check(f"census_dependency_{mode}_fails_harness",
                  proc.returncode != 0 and "census" in (proc.stdout + proc.stderr),
                  f"rc={proc.returncode} out={proc.stdout[-200:]} "
                  f"err={proc.stderr[-200:]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{passes} passed, {len(failures)} failed")
    for f in failures:
        print(f"  - {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
