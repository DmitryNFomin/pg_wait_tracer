#!/usr/bin/env python3
"""test_demo_rehearsal_orchestrator_lib.py -- unit tests for
tests/demo_rehearsal_orchestrator_lib.py (issue #176). Pure Python, no
network, no VM, no subprocess -- runs on the Mac:
  python3 tests/test_demo_rehearsal_orchestrator_lib.py

Covers the bypass suite scripts/demo-rehearsal.sh's rewrite had to close
(issue #176's "what makes this silently report success"):
  - a missing completion marker treated as done
  - an empty/foreign results directory copied over a previous run's
  - a truncated summary.json (interrupted transfer) accepted as whole
  - a remote run that died mid-capture read as anything but a failure
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo_rehearsal_orchestrator_lib as lib

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def check_raises(fn, msg):
    try:
        fn()
        check(False, msg)
    except lib.OrchestratorError:
        check(True, msg)


# ── parse_state_line ─────────────────────────────────────────────────────

def test_parse_state_line_finished():
    r = lib.parse_state_line("STATE=finished RC=0")
    check(r == {"state": "finished", "rc": 0}, "parse-state: finished RC=0")


def test_parse_state_line_finished_nonzero():
    r = lib.parse_state_line("STATE=finished RC=1")
    check(r == {"state": "finished", "rc": 1}, "parse-state: finished RC=1")


def test_parse_state_line_running_died_not_started():
    for s in ("running", "died", "not-started", "unreachable"):
        r = lib.parse_state_line(f"STATE={s} RC=-")
        check(r == {"state": s, "rc": None}, f"parse-state: {s} RC=- -> rc None")


def test_parse_state_line_garbage_raises():
    check_raises(lambda: lib.parse_state_line(""),
                 "parse-state: empty line raises (ssh hiccup must not look like a state)")
    check_raises(lambda: lib.parse_state_line("connection reset by peer"),
                 "parse-state: garbled ssh output raises, not silently 'not-started'")
    check_raises(lambda: lib.parse_state_line("STATE=bogus RC=-"),
                 "parse-state: unknown state name raises")
    check_raises(lambda: lib.parse_state_line("STATE=finished RC=abc"),
                 "parse-state: non-numeric RC raises")


# ── compute_wait_budget_s ────────────────────────────────────────────────

def test_wait_budget_fresh_launch_is_full_window():
    start = 1_000_000
    got = lib.compute_wait_budget_s(start, start, duration_min=35, build_buffer_s=900)
    check(got == 35 * 60 + 900,
          f"wait-budget: fresh launch (now==start) is the full window+buffer (got {got})")


def test_wait_budget_collect_reattach_is_remaining_only():
    start = 1_000_000
    now = start + 600  # 10 minutes after launch
    got = lib.compute_wait_budget_s(start, now, duration_min=35, build_buffer_s=900)
    check(got == 35 * 60 + 900 - 600,
          f"wait-budget: --collect reattach 10min later waits the REMAINING budget only (got {got})")


def test_wait_budget_never_negative():
    start = 1_000_000
    now = start + 10**9  # absurdly far in the future
    got = lib.compute_wait_budget_s(start, now, duration_min=3, build_buffer_s=900)
    check(got == 0, f"wait-budget: past the deadline clamps to 0, not negative (got {got})")


# ── validate_results_dir: the bypass suite ───────────────────────────────

def _write_summary(d, ok=True, failed=None):
    with open(os.path.join(d, "summary.json"), "w") as f:
        json.dump({"ok": ok, "failed": failed or [], "passes": {}, "checks": {}}, f)


def _write_run_id(d, run_id):
    with open(os.path.join(d, "run.id"), "w") as f:
        f.write(str(run_id))


def test_validate_missing_directory_bypass():
    ok, reason = lib.validate_results_dir("/nonexistent/does/not/exist", 123)
    check(not ok, f"validate-results BYPASS CASE: missing directory rejected ({reason})")


def test_validate_empty_directory_bypass():
    # "an empty results directory copied over a previous run's" -- the
    # exact bypass case issue #176 names.
    with tempfile.TemporaryDirectory() as d:
        ok, reason = lib.validate_results_dir(d, 123)
        check(not ok, f"validate-results BYPASS CASE: empty directory rejected ({reason})")


def test_validate_missing_marker_bypass():
    # "a missing completion marker treated as done" -- no run.id at all.
    with tempfile.TemporaryDirectory() as d:
        _write_summary(d)
        ok, reason = lib.validate_results_dir(d, 123)
        check(not ok, f"validate-results BYPASS CASE: missing run.id (no marker) rejected ({reason})")


def test_validate_stale_run_id_bypass():
    # A directory left over from a PREVIOUS invocation must never pass as
    # this invocation's result.
    with tempfile.TemporaryDirectory() as d:
        _write_summary(d)
        _write_run_id(d, 111)
        ok, reason = lib.validate_results_dir(d, 999)
        check(not ok, f"validate-results BYPASS CASE: stale/foreign run.id rejected ({reason})")


def test_validate_truncated_summary_bypass():
    # "a transfer interrupted midway" -- summary.json cut off mid-write.
    with tempfile.TemporaryDirectory() as d:
        _write_run_id(d, 42)
        with open(os.path.join(d, "summary.json"), "w") as f:
            f.write('{"ok": true, "fail')  # truncated, invalid JSON
        ok, reason = lib.validate_results_dir(d, 42)
        check(not ok, f"validate-results BYPASS CASE: truncated summary.json rejected ({reason})")


def test_validate_missing_required_keys_bypass():
    with tempfile.TemporaryDirectory() as d:
        _write_run_id(d, 42)
        with open(os.path.join(d, "summary.json"), "w") as f:
            json.dump({"unrelated": True}, f)
        ok, reason = lib.validate_results_dir(d, 42)
        check(not ok, f"validate-results BYPASS CASE: summary.json missing ok/failed rejected ({reason})")


def test_validate_complete_fresh_result_accepted():
    with tempfile.TemporaryDirectory() as d:
        _write_summary(d, ok=True)
        _write_run_id(d, 42)
        ok, reason = lib.validate_results_dir(d, 42)
        check(ok, f"validate-results: complete, fresh, matching-run.id directory accepted ({reason})")


def test_validate_complete_failed_result_still_accepted():
    # A FAILED rehearsal (real product bug caught) is still a complete,
    # trustworthy transfer -- validity is about the TRANSFER, not the
    # rehearsal's own verdict.
    with tempfile.TemporaryDirectory() as d:
        _write_summary(d, ok=False, failed=["early/timeline"])
        _write_run_id(d, 7)
        ok, reason = lib.validate_results_dir(d, 7)
        check(ok, f"validate-results: complete transfer of a FAILED rehearsal still accepted ({reason})")


# ── decide_outcome ────────────────────────────────────────────────────────

def test_decide_outcome_success():
    code, verdict = lib.decide_outcome("finished", 0, True)
    check(code == 0, f"decide-outcome: finished/rc=0/results_ok -> exit 0 (got {code}: {verdict})")


def test_decide_outcome_finished_nonzero_rc_fails():
    code, verdict = lib.decide_outcome("finished", 1, True)
    check(code == 1, f"decide-outcome: finished/rc=1 -> exit 1 even with valid results (got {code})")


def test_decide_outcome_finished_but_results_invalid_fails():
    # "empty results directory copied over a previous run's" must not
    # produce a summary that looks whole -- rc==0 alone is not success.
    code, verdict = lib.decide_outcome("finished", 0, False)
    check(code == 1, f"decide-outcome BYPASS CASE: rc=0 but results_ok=False -> still exit 1 (got {code})")


def test_decide_outcome_running_is_not_a_failure():
    code, verdict = lib.decide_outcome("running", None, False)
    check(code == 2, f"decide-outcome: still running -> distinct code 2, not success or failure (got {code})")


def test_decide_outcome_died_fails():
    code, verdict = lib.decide_outcome("died", None, False)
    check(code == 1 and "died" in verdict,
          f"decide-outcome BYPASS CASE: died -> exit 1, verdict says died (got {code}: {verdict})")


def test_decide_outcome_not_started_fails():
    code, verdict = lib.decide_outcome("not-started", None, False)
    check(code == 1, f"decide-outcome BYPASS CASE: never provisioned/started -> exit 1 (got {code})")


def test_decide_outcome_unreachable_fails():
    code, verdict = lib.decide_outcome("unreachable", None, False)
    check(code == 1, f"decide-outcome: unreachable VM -> exit 1, never silently retried forever (got {code})")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"-- {name}")
            fn()
    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
