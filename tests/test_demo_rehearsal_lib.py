#!/usr/bin/env python3
"""test_demo_rehearsal_lib.py -- unit tests for tests/demo_rehearsal_lib.py
(issue #157). Pure Python, no browser, no network, no subprocess -- can run
on the Mac (python3 tests/test_demo_rehearsal_lib.py); wired into
tests/unit_tests.list the same way tests/test_ui_live_smoke_lib.py is (runs
in the C-unit-suite tier, CI/nightly/box-check, not scripts/check.sh -- see
that file's own header for why).

Usage: python3 tests/test_demo_rehearsal_lib.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo_rehearsal_lib as lib

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


# ── schedule_passes ──────────────────────────────────────────────────────

def test_schedule_passes_three_spread_across_window():
    passes = lib.schedule_passes(2100.0, pass_budget_s=330.0)
    check([n for n, _ in passes] == ["early", "middle", "late"],
          "schedule_passes: names in order early/middle/late")
    offsets = [o for _, o in passes]
    check(offsets == sorted(offsets), "schedule_passes: offsets non-decreasing")
    check(offsets[0] >= 60.0, f"early pass starts after warmup (got {offsets[0]})")
    for a, b in zip(offsets, offsets[1:]):
        check(b - a >= 330.0,
              f"consecutive passes are >= pass_budget_s apart ({a} -> {b}), "
              "so one pass always finishes before the next starts")
    check(offsets[-1] + 330.0 <= 2100.0 - 90.0,
          f"late pass ({offsets[-1]}) + its budget fits before the tail buffer")
    # middle pass roughly in the middle of the window, not bunched with
    # either edge.
    check(offsets[1] > offsets[0] + 100 and offsets[1] < offsets[-1] - 100,
          f"middle pass ({offsets[1]}) is meaningfully between early/late "
          f"({offsets[0]}/{offsets[-1]})")


def test_schedule_passes_self_test_short_duration():
    # The self-test invocation (DURATION_MIN=3, issue #157) with a much
    # smaller per-pass budget must still schedule three distinct,
    # non-overlapping passes.
    passes = lib.schedule_passes(180.0, pass_budget_s=20.0, warmup_s=15.0,
                                 tail_buffer_s=20.0)
    check(len(passes) == 3, "self-test window still yields 3 passes")
    offsets = [o for _, o in passes]
    check(offsets[0] >= 15.0, f"first pass starts no earlier than warmup_s (got {offsets[0]})")
    for a, b in zip(offsets, offsets[1:]):
        check(b - a >= 20.0, f"self-test passes still >= pass_budget_s apart ({a} -> {b})")
    check(offsets[-1] + 20.0 <= 180.0 - 20.0,
          "last self-test pass still respects the tail buffer")


def test_schedule_passes_too_short_raises():
    raised = False
    try:
        lib.schedule_passes(60.0, pass_budget_s=330.0)
    except ValueError:
        raised = True
    check(raised, "schedule_passes: too-short duration raises ValueError "
                 "(never silently drops coverage)")


def test_schedule_passes_single_pass_exact_fit():
    # duration_s sized to exactly the tight (zero-slack) schedule: the one
    # pass must land exactly at warmup_s.
    passes = lib.schedule_passes(50.0, pass_budget_s=30.0, warmup_s=10.0,
                                 tail_buffer_s=10.0, names=("only",))
    check(passes == [("only", 10.0)],
          f"zero-slack single-name schedule is [(name, warmup_s)] (got {passes})")


def test_schedule_passes_single_pass_with_slack():
    passes = lib.schedule_passes(300.0, pass_budget_s=30.0, warmup_s=10.0,
                                 tail_buffer_s=10.0, names=("only",))
    name, offset = passes[0]
    check(name == "only", "single pass keeps its name")
    check(offset >= 10.0, f"offset never earlier than warmup_s (got {offset})")
    check(offset + 30.0 <= 300.0 - 10.0,
          f"offset + budget still respects the tail buffer (got {offset})")


# ── plan_passes ───────────────────────────────────────────────────────────

def test_plan_passes_real_run_uses_max_ticks_and_three_passes():
    ticks, passes = lib.plan_passes(2100.0)  # DURATION_MIN=35 default
    check(ticks == 6, f"a real 35-min run gets the full MIN_TICKS=6 (got {ticks})")
    check([n for n, _ in passes] == ["early", "middle", "late"],
          "a real 35-min run gets all 3 passes")


def test_plan_passes_self_test_degrades_gracefully():
    ticks, passes = lib.plan_passes(180.0)  # DURATION_MIN=3 self-test
    check(ticks >= 1, f"self-test still resolves to a positive tick count (got {ticks})")
    check(len(passes) >= 1, f"self-test still resolves to at least one pass (got {passes})")
    check(ticks < 6 or len(passes) < 3,
          "a 3-minute window cannot fit the full 6-tick/3-pass real-run plan "
          "-- plan_passes must have degraded ticks and/or pass count, not "
          "silently kept the real-run plan")


def test_plan_passes_impossibly_short_raises():
    raised = False
    try:
        lib.plan_passes(5.0)
    except ValueError:
        raised = True
    check(raised, "an impossibly short duration raises rather than "
                 "returning a plan that cannot actually run")


# ── time_model_conservation ──────────────────────────────────────────────

def test_conservation_holds_exactly():
    rows = [
        {"name": "DB Time", "ms": 1000.0, "indent": 0},
        {"name": "CPU*", "ms": 600.0, "indent": 1},
        {"name": "Lock", "ms": 300.0, "indent": 1},
        {"name": "relation", "ms": 300.0, "indent": 2},
        {"name": "IO", "ms": 100.0, "indent": 1},
    ]
    ok, detail = lib.time_model_conservation(rows, 1000.0)
    check(ok, f"CPU*+Lock+IO == db_time_ms conserves exactly ({detail})")


def test_conservation_within_tolerance():
    rows = [
        {"name": "CPU*", "ms": 500.0, "indent": 1},
        {"name": "Lock", "ms": 495.0, "indent": 1},
    ]
    # 995 vs 1000 = 0.5% gap, under the 1% default tolerance.
    ok, detail = lib.time_model_conservation(rows, 1000.0)
    check(ok, f"0.5% gap is within the 1% tolerance ({detail})")


def test_conservation_missing_offcpu_fails():
    # The summary-path finding this harness exists to catch: a real
    # residual (Off-CPU*) entirely missing from the rows.
    rows = [
        {"name": "CPU*", "ms": 400.0, "indent": 1},
        {"name": "Lock", "ms": 300.0, "indent": 1},
    ]
    ok, detail = lib.time_model_conservation(rows, 1000.0)
    check(not ok, f"a 30% missing residual fails conservation ({detail})")
    check("gap=300.0ms" in detail, f"detail states the actual gap (got: {detail})")


def test_conservation_ignores_indent_0_and_2():
    # Only indent==1 rows are summed -- indent 0 is DB Time itself (would
    # double the total), indent 2 rows are already inside their indent-1
    # parent's ms (would double-count that class).
    rows = [
        {"name": "DB Time", "ms": 1000.0, "indent": 0},
        {"name": "CPU*", "ms": 1000.0, "indent": 1},
        {"name": "cpu-sub", "ms": 1000.0, "indent": 2},
    ]
    ok, detail = lib.time_model_conservation(rows, 1000.0)
    check(ok, f"indent 0/2 rows excluded from the sum ({detail})")


def test_conservation_empty_window_ok():
    ok, detail = lib.time_model_conservation([], 0.0)
    check(ok, f"an empty/idle window (db_time_ms<=0) trivially conserves ({detail})")


# ── build_time_model_check ────────────────────────────────────────────────

def test_time_model_check_recent_raw_path_ok():
    result = lib.build_time_model_check(
        recent_ok=True, recent_detail="recent detail", recent_used_raw_path=True,
        full_ok=True, full_detail="full detail", full_used_raw_path=False)
    check(result["ok"], "recent window conserves AND used the raw path -> ok")
    check(result["recent_window"]["compute_path"] == "raw",
          "recent_window records compute_path='raw'")
    check(result["full_window"]["compute_path"] == "summary",
          "full_window records compute_path='summary' (the expected/normal case)")
    check("does not gate" in result["full_window"]["note"],
          "full_window's note says it does not gate `ok`")


def test_time_model_check_full_window_failure_does_not_gate():
    # The whole point of the fix: a full-window (summary-path) "failure"
    # must NOT fail the overall check -- it is structurally incapable of
    # being a real signal, per the note above.
    result = lib.build_time_model_check(
        recent_ok=True, recent_detail="ok", recent_used_raw_path=True,
        full_ok=False, full_detail="full-window mismatch", full_used_raw_path=False)
    check(result["ok"],
          "a full-window conservation 'failure' alone does not fail the check")


def test_time_model_check_recent_conservation_failure_fails():
    result = lib.build_time_model_check(
        recent_ok=False, recent_detail="30% gap", recent_used_raw_path=True,
        full_ok=True, full_detail="ok", full_used_raw_path=False)
    check(not result["ok"],
          "a real conservation gap on the recent/raw-path window fails the check")


def test_time_model_check_recent_not_raw_path_fails_loudly():
    # If the short window somehow did NOT get the raw path (e.g. server.c's
    # should_use_summaries threshold changes under us), the check must fail
    # rather than silently trust what could be an equally vacuous result.
    result = lib.build_time_model_check(
        recent_ok=True, recent_detail="looks fine", recent_used_raw_path=False,
        full_ok=True, full_detail="ok", full_used_raw_path=False)
    check(not result["ok"],
          "recent window NOT using the raw path fails the check even though "
          "its own conservation math reported ok=True")
    check(result["recent_window"]["compute_path"] == "summary",
          "the compute_path actually observed is reported, not assumed")


# ── waterfall_latency_ok ──────────────────────────────────────────────────

def test_waterfall_latency_under_threshold():
    check(lib.waterfall_latency_ok(9.9), "9.9s is under the 10s threshold")


def test_waterfall_latency_over_threshold():
    check(not lib.waterfall_latency_ok(10.1), "10.1s exceeds the 10s threshold")


def test_waterfall_latency_none_fails():
    check(not lib.waterfall_latency_ok(None), "None elapsed (query never returned) fails")


# ── daemon_log_clean ──────────────────────────────────────────────────────

def test_daemon_log_clean_no_findings():
    log = "WARN: target effective CPU capacity unknown\nsome info line\n"
    ok, errors, warnings = lib.daemon_log_clean(log)
    check(ok, "no ERROR:/FATAL: lines -> ok")
    check(errors == [], "no error lines collected")
    check(warnings == [], "'CPU capacity unknown' is not a degraded-tier marker")


def test_daemon_log_clean_error_fails():
    log = "ui_live_smoke: control socket ready\nERROR: window too large\n"
    ok, errors, warnings = lib.daemon_log_clean(log)
    check(not ok, "an ERROR: line fails the check")
    check(errors == ["ERROR: window too large"], f"error line captured verbatim (got {errors})")


def test_daemon_log_clean_fatal_fails():
    ok, errors, warnings = lib.daemon_log_clean("FATAL: BPF load failed: x\n")
    check(not ok, "a FATAL: line fails the check")


def test_daemon_log_clean_degraded_warn_announced_not_failed():
    log = "WARN: CPU accounting: LEGACY gap-inference -- degraded mode\n"
    ok, errors, warnings = lib.daemon_log_clean(log)
    check(ok, "a degraded-tier WARN alone does not fail the check (it was logged)")
    check(len(warnings) == 1, f"but it IS captured for the report (got {warnings})")


def test_daemon_log_clean_stray_error_word_in_query_not_matched():
    # A benign line that merely CONTAINS the word must not match -- only a
    # line starting with the literal prefix does (the daemon's own
    # fprintf(stderr, "ERROR: ...") convention).
    log = "note: query text mentions an error code column\n"
    ok, errors, warnings = lib.daemon_log_clean(log)
    check(ok and errors == [], "a line merely containing 'error' (not the prefix) is not flagged")


# ── build_demo_summary / verdict_line ────────────────────────────────────

def _tab_result(ok):
    return {"ok": ok}


def test_build_demo_summary_all_pass():
    pass_results = [
        {"pass": "early", "tabs": {"overview": _tab_result(True)}},
        {"pass": "middle", "tabs": {"overview": _tab_result(True)}},
    ]
    extra = {"time_model_conserves": {"ok": True}}
    summary = lib.build_demo_summary(pass_results, extra)
    check(summary["ok"], "all-pass summary is ok")
    check(summary["failed"] == [], "no failed entries")
    check(lib.verdict_line(summary) == "DEMO REHEARSAL: PASS",
          "verdict line PASS")


def test_build_demo_summary_ignores_known_failing_exemption():
    # A tab result carrying known_failing=True but ok=False (exactly what
    # ui_live_smoke_lib.build_tab_result emits for timeline/waterfall) must
    # still fail the demo-rehearsal summary -- issue #157: no known-failing
    # exemption here, ever.
    kf_tab = {"ok": False, "known_failing": True}
    pass_results = [{"pass": "early", "tabs": {"waterfall": kf_tab}}]
    summary = lib.build_demo_summary(pass_results, {"c": {"ok": True}})
    check(not summary["ok"],
          "a known_failing=True tab with raw ok=False still fails the rehearsal")
    check(summary["failed"] == ["early/waterfall"],
          f"failed list names the pass/tab (got {summary['failed']})")


def test_build_demo_summary_extra_check_failure():
    pass_results = [{"pass": "early", "tabs": {"overview": _tab_result(True)}}]
    extra = {"waterfall_query_latency": {"ok": False, "elapsed_s": 12.0}}
    summary = lib.build_demo_summary(pass_results, extra)
    check(not summary["ok"], "a failed extra check fails the summary")
    check("waterfall_query_latency" in summary["failed"],
          "failed extra check named in failed list")
    check(lib.verdict_line(summary).startswith("DEMO REHEARSAL: FAIL"),
          "verdict line FAIL")


def test_build_demo_summary_requires_passes_and_checks():
    check(not lib.build_demo_summary([], {"c": {"ok": True}})["ok"],
          "no passes at all is never a PASS")
    check(not lib.build_demo_summary(
        [{"pass": "early", "tabs": {"overview": _tab_result(True)}}], {})["ok"],
          "no extra checks at all is never a PASS")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"--- {name} ---")
            fn()

    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
