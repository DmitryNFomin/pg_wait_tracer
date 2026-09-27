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


def test_conservation_zero_db_time_fails():
    # BYPASS-SUITE CASE (issue #157 rehearsal-gate audit, owner finding
    # 2026-09-27): this used to be test_conservation_empty_window_ok,
    # which asserted db_time_ms<=0 was a trivial PASS ("nothing to
    # conserve"). In THIS harness (continuous load for the whole capture)
    # a zero-DB-Time window is exactly "a capture with zero samples" --
    # void evidence, never a clean pass. Demonstrated red against the
    # pre-fix lib: it returned (True, "...nothing to conserve").
    ok, detail = lib.time_model_conservation([], 0.0)
    check(not ok, f"a zero-DB-Time window must FAIL, not vacuously pass ({detail})")
    check("floor" in detail, f"detail names the floor that tripped (got: {detail})")


def test_conservation_floating_noise_below_floor_fails():
    # A db_time_ms that is technically > 0.0 only by floating-point noise
    # (e.g. accumulated rounding) must not slip past a literal ">0" check.
    ok, detail = lib.time_model_conservation([], 1e-9)
    check(not ok, f"a near-zero db_time_ms (float noise) must still fail ({detail})")


def test_conservation_just_above_floor_is_evaluated_normally():
    rows = [{"name": "CPU (running)", "ms": 2.0, "indent": 1}]
    ok, detail = lib.time_model_conservation(rows, 2.0)
    check(ok, f"db_time_ms clearing the floor is evaluated by the normal ratio ({detail})")


# ── time_model_over_attribution_ok / time_model_offcpu_cap_ok ────────────

def test_over_attribution_clean_trace_ok():
    ok, detail = lib.time_model_over_attribution_ok(
        wait_gap_cpu_ms=0.05, cpu_clamped_ms=0.0, db_time_ms=10000.0)
    check(ok, f"negligible wait_gap_cpu_ms/cpu_clamped_ms pass ({detail})")


def test_over_attribution_wait_gap_cpu_fails():
    # BYPASS-SUITE CASE: CPU measured during a wait-labeled gap (should be
    # ~0) at 5% of db_time_ms -- an over-attribution the identity-sum check
    # alone cannot see (the identity sums whichever bucket the CPU landed
    # in, so it always closes).
    ok, detail = lib.time_model_over_attribution_ok(
        wait_gap_cpu_ms=500.0, cpu_clamped_ms=0.0, db_time_ms=10000.0)
    check(not ok, f"wait_gap_cpu_ms at 5% of db_time_ms fails ({detail})")


def test_over_attribution_cpu_clamped_fails():
    ok, detail = lib.time_model_over_attribution_ok(
        wait_gap_cpu_ms=0.0, cpu_clamped_ms=50.0, db_time_ms=10000.0)
    check(not ok, f"cpu_clamped_ms at 0.5% of db_time_ms fails the 0.1% bound ({detail})")


def test_offcpu_cap_ok_under_cap():
    ok, detail = lib.time_model_offcpu_cap_ok(
        offcpu_ms=400.0, db_time_ms=10000.0, has_measured_cpu=True)
    check(ok, f"4% Off-CPU* is under the 10% cap ({detail})")


def test_offcpu_cap_fails_over_cap():
    # BYPASS-SUITE CASE (the harder finding): a dropped wait class's time
    # is absorbed into the Off-CPU* residual by construction (src/compute.c
    # -- see time_model_conservation's own docstring), so it is
    # over-sized rather than making the identity fail. This is the check
    # with real detection power against that bug class.
    ok, detail = lib.time_model_offcpu_cap_ok(
        offcpu_ms=4000.0, db_time_ms=10000.0, has_measured_cpu=True)
    check(not ok, f"40% Off-CPU* (a dropped-class-sized residual) fails the 10% cap ({detail})")


def test_offcpu_cap_not_applicable_without_measured_cpu():
    ok, detail = lib.time_model_offcpu_cap_ok(
        offcpu_ms=9999.0, db_time_ms=10000.0, has_measured_cpu=False)
    check(ok, f"legacy/sampled tier (no measured CPU) reports ok, labeled not applicable ({detail})")
    check("not applicable" in detail or "no Off-CPU*" in detail,
          f"detail says WHY it is vacuously ok, not just ok ({detail})")


# ── evaluate_time_model_window ────────────────────────────────────────────

def test_evaluate_window_clean_trace_ok():
    rows = [
        {"name": "CPU (running)", "ms": 6000.0, "indent": 1},
        {"name": "Lock", "ms": 3600.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 400.0, "indent": 1},
    ]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=10000.0, wait_gap_cpu_ms=1.0, cpu_clamped_ms=0.0,
        offcpu_ms=400.0, has_measured_cpu=True, used_raw_path=True)
    check(result["ok"], f"a clean, fully-attributed trace passes every sub-check ({result})")


def test_evaluate_window_dropped_wait_class_caught_by_offcpu_cap():
    # BYPASS-SUITE CASE (the reason this task exists): an entire wait class
    # (Lock, ~3000ms) is dropped from `rows`; src/compute.c's residual
    # definition means its time is absorbed into Off-CPU* and the identity
    # still closes EXACTLY. The identity sub-check alone says ok=True; the
    # combined gate must still say ok=False via the Off-CPU* cap.
    rows = [
        {"name": "CPU (running)", "ms": 3000.0, "indent": 1},
        {"name": "IO", "ms": 3000.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 4000.0, "indent": 1},
    ]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=10000.0, wait_gap_cpu_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=4000.0, has_measured_cpu=True, used_raw_path=True)
    check(result["conservation"]["ok"],
          f"the identity alone still closes exactly for this dropped-class case ({result})")
    check(not result["ok"],
          f"the combined gate still fails via the Off-CPU* cap ({result})")
    check(not result["offcpu_cap"]["ok"], "offcpu_cap sub-check is the one that failed")


def test_evaluate_window_not_raw_path_fails_loudly():
    rows = [{"name": "CPU*", "ms": 1000.0, "indent": 1}]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=1000.0, wait_gap_cpu_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=False)
    check(not result["ok"],
          "summary compute path fails the window even though the identity closes")
    check(result["compute_path"] == "summary", "compute_path reported accurately")


def test_evaluate_window_zero_db_time_fails_every_subcheck():
    result = lib.evaluate_time_model_window(
        [], db_time_ms=0.0, wait_gap_cpu_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=True)
    check(not result["ok"], f"a zero-DB-Time window fails the combined gate ({result})")
    check(not result["over_attribution"]["ok"],
          "over_attribution is not vacuously ok below the DB-Time floor")
    check(not result["offcpu_cap"]["ok"],
          "offcpu_cap is not vacuously ok below the DB-Time floor")


# ── conservation_sample_interval_s / build_demo_conservation_check ───────

def test_sample_interval_real_run_uses_target():
    interval = lib.conservation_sample_interval_s(2100.0)  # 35 min
    check(interval == 300.0, f"a real 35-min run samples every 5 minutes (got {interval})")


def test_sample_interval_self_test_scales_down():
    interval = lib.conservation_sample_interval_s(180.0)  # 3 min self-test
    check(interval < 300.0, f"a 3-min self-test must not use the 5-min real-run interval (got {interval})")
    check(180.0 / interval >= 3, f"self-test still yields >= 3 samples (got {180.0/interval:.1f})")


def test_sample_interval_nonpositive_duration_raises():
    raised = False
    try:
        lib.conservation_sample_interval_s(0.0)
    except ValueError:
        raised = True
    check(raised, "a zero/negative duration raises rather than returning a bogus interval")


def test_conservation_samples_empty_list_is_void():
    # BYPASS-SUITE CASE: no in-capture samples were EVER gathered (e.g. the
    # sampler thread crashed before its first iteration, or was never
    # started) -- must never read as a vacuous PASS just because
    # full_window_result looks fine.
    result = lib.build_demo_conservation_check([], {"ok": True, "detail": "n/a"})
    check(not result["ok"], f"zero samples is void, never a vacuous PASS ({result})")


def test_conservation_samples_one_bad_sample_fails_even_if_others_pass():
    samples = [
        {"offset_s": 0.0, "ok": True},
        {"offset_s": 300.0, "ok": False},
        {"offset_s": 600.0, "ok": True},
    ]
    result = lib.build_demo_conservation_check(samples, {"ok": True})
    check(not result["ok"],
          "one failing sample fails the whole check -- not an average, not just the last one")
    check(result["failed_offsets_s"] == [300.0],
          f"the failing offset is named (got {result['failed_offsets_s']})")


def test_conservation_samples_all_pass():
    samples = [{"offset_s": 0.0, "ok": True}, {"offset_s": 300.0, "ok": True}]
    result = lib.build_demo_conservation_check(samples, {"ok": True})
    check(result["ok"], "every sample passing -> ok")
    check(result["num_samples"] == 2, "num_samples reported")


# ── workload_signature_present_ok / aas_floor_ok (criteria doc §2) ───────

def test_workload_signature_both_present_ok():
    rows = [
        {"name": "Lock:relation", "ms": 300.0, "indent": 2},
        {"name": "Timeout:PgSleep", "ms": 200.0, "indent": 2},
    ]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(ok, f"both signature events present with real time ({detail})")


def test_workload_signature_missing_one_fails():
    # BYPASS-SUITE CASE (criteria doc §2): we captured something other than
    # the intended workload -- Timeout:PgSleep never shows up at all.
    rows = [{"name": "Lock:relation", "ms": 300.0, "indent": 2}]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(not ok, f"a missing required event fails ({detail})")
    check("Timeout:PgSleep" in detail, f"detail names what's missing ({detail})")


def test_workload_signature_zero_time_fails():
    # Present in name only, zero real time -- must not count as "present".
    rows = [
        {"name": "Lock:relation", "ms": 0.0, "indent": 2},
        {"name": "Timeout:PgSleep", "ms": 150.0, "indent": 2},
    ]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(not ok, f"a zero-time row does not count as present ({detail})")


def test_workload_signature_empty_rows_fails():
    ok, detail = lib.workload_signature_present_ok([])
    check(not ok, f"empty rows (e.g. an idle/empty window) fails ({detail})")


def test_aas_floor_ok_above_floor():
    ok, detail = lib.aas_floor_ok(1.2)
    check(ok, f"aas=1.2 clears the 0.5 floor ({detail})")


def test_aas_floor_below_floor_fails():
    # BYPASS-SUITE CASE: an AAS reading near zero is void the same way a
    # near-zero db_time_ms is.
    ok, detail = lib.aas_floor_ok(0.1)
    check(not ok, f"aas=0.1 is below the provisional 0.5 floor ({detail})")


def test_aas_floor_none_fails():
    ok, detail = lib.aas_floor_ok(None)
    check(not ok, f"a missing aas value fails, is not treated as fine ({detail})")


# ── evaluate_time_model_window with the criteria-doc #2 extra floors ─────

def test_evaluate_window_workload_signature_and_aas_when_requested():
    rows = [
        {"name": "CPU (running)", "ms": 6000.0, "indent": 1},
        {"name": "Lock", "ms": 3600.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 400.0, "indent": 1},
        {"name": "Lock:relation", "ms": 3000.0, "indent": 2},
        {"name": "Timeout:PgSleep", "ms": 500.0, "indent": 2},
    ]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=10000.0, wait_gap_cpu_ms=1.0, cpu_clamped_ms=0.0,
        offcpu_ms=400.0, has_measured_cpu=True, used_raw_path=True,
        aas=1.5, check_workload_signature=True, check_aas_floor=True)
    check(result["ok"], f"a clean sample with real workload signature and healthy AAS passes ({result})")
    check(result["workload_signature"]["ok"], "workload_signature sub-check recorded ok")
    check(result["aas_floor"]["ok"], "aas_floor sub-check recorded ok")


def test_evaluate_window_missing_workload_signature_fails_when_requested():
    # BYPASS-SUITE CASE: an otherwise-clean window whose workload signature
    # is missing must still fail once that floor is requested.
    rows = [
        {"name": "CPU (running)", "ms": 6000.0, "indent": 1},
        {"name": "Lock", "ms": 3600.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 400.0, "indent": 1},
    ]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=10000.0, wait_gap_cpu_ms=1.0, cpu_clamped_ms=0.0,
        offcpu_ms=400.0, has_measured_cpu=True, used_raw_path=True,
        aas=1.5, check_workload_signature=True, check_aas_floor=True)
    check(not result["ok"], f"missing workload signature fails the window ({result})")
    check(not result["workload_signature"]["ok"], "workload_signature sub-check recorded not ok")


def test_evaluate_window_skips_extra_floors_by_default():
    # The whole-capture-window diagnostic call site does NOT request these
    # (see evaluate_time_model_window's own docstring) -- confirm the
    # default really is "off", not silently on.
    rows = [{"name": "CPU (running)", "ms": 1000.0, "indent": 1}]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=1000.0, wait_gap_cpu_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=True)
    check("workload_signature" not in result, "workload_signature absent when not requested")
    check("aas_floor" not in result, "aas_floor absent when not requested")


# ── cross_tab_db_time_agreement_ok / freshness_ok (criteria doc §5) ──────

def test_cross_tab_agreement_within_tolerance_ok():
    ok, detail = lib.cross_tab_db_time_agreement_ok(10000.0, 10050.0)
    check(ok, f"0.5% disagreement is within the 1% tolerance ({detail})")


def test_cross_tab_agreement_disagreement_fails():
    # BYPASS-SUITE CASE: two tabs computing DB Time for the identical
    # window disagree by 10% -- a real bookkeeping/denominator bug.
    ok, detail = lib.cross_tab_db_time_agreement_ok(10000.0, 11000.0)
    check(not ok, f"a 10% cross-tab disagreement fails ({detail})")


def test_cross_tab_agreement_zero_both_sides_fails():
    # BYPASS-SUITE CASE: 0 ~= 0 must not satisfy cross-tab agreement any
    # more than it satisfies the identity check.
    ok, detail = lib.cross_tab_db_time_agreement_ok(0.0, 0.0)
    check(not ok, f"both sides at zero DB Time is void, not agreement ({detail})")


def test_cross_tab_agreement_missing_side_fails():
    ok, detail = lib.cross_tab_db_time_agreement_ok(10000.0, None)
    check(not ok, f"a missing side fails rather than being skipped ({detail})")


def test_freshness_recent_bucket_ok():
    now_ns = 1_000_000_000_000
    to_ns = now_ns - 3_000_000_000  # 3s old, well under the 10s bound
    ok, detail = lib.freshness_ok(now_ns, to_ns)
    check(ok, f"a 3s-old newest bucket is fresh ({detail})")


def test_freshness_stale_bucket_fails():
    # BYPASS-SUITE CASE: a capture that stalled -- to_ns stopped advancing
    # while now_ns kept moving, well past the 2-tick (10s) bound.
    now_ns = 1_000_000_000_000
    to_ns = now_ns - 60_000_000_000  # 60s old
    ok, detail = lib.freshness_ok(now_ns, to_ns)
    check(not ok, f"a 60s-old newest bucket fails the 10s freshness bound ({detail})")


def test_freshness_missing_fields_fails():
    ok, detail = lib.freshness_ok(None, None)
    check(not ok, f"missing now_ns/to_ns fails rather than being skipped ({detail})")


# ── build_demo_summary: expected_tabs_per_pass floor (criteria doc §2) ──

def test_build_demo_summary_all_tabs_reached_with_expected_count_ok():
    pass_results = [{"pass": "early",
                     "tabs": {f"tab{i}": _tab_result(True) for i in range(11)}}]
    summary = lib.build_demo_summary(pass_results, {"c": {"ok": True}},
                                     expected_tabs_per_pass=11)
    check(summary["ok"], "exactly 11/11 tabs reached passes the floor")


def test_build_demo_summary_missing_tabs_fails_with_expected_count():
    # BYPASS-SUITE CASE (criteria doc §2 "all 11 tabs reached"): a pass that
    # only reached 9 of 11 tabs (e.g. the walk aborted early) must fail even
    # though every reached tab individually passed.
    pass_results = [{"pass": "early",
                     "tabs": {f"tab{i}": _tab_result(True) for i in range(9)}}]
    summary = lib.build_demo_summary(pass_results, {"c": {"ok": True}},
                                     expected_tabs_per_pass=11)
    check(not summary["ok"], f"9/11 tabs reached fails when 11 are expected ({summary})")
    check(any("incomplete_pass" in f for f in summary["failed"]),
          f"failed list names the incomplete pass ({summary['failed']})")


def test_build_demo_summary_expected_tabs_none_skips_the_check():
    # Backward compatible: callers that do not know/pass the expected count
    # (existing tests above) are not newly broken by this floor.
    pass_results = [{"pass": "early", "tabs": {"overview": _tab_result(True)}}]
    summary = lib.build_demo_summary(pass_results, {"c": {"ok": True}})
    check(summary["ok"], "expected_tabs_per_pass=None does not gate on tab count")


# ── capture_has_events_ok ─────────────────────────────────────────────────

def test_capture_has_events_positive_ok():
    ok, detail = lib.capture_has_events_ok(50000)
    check(ok, f"a positive event count passes ({detail})")


def test_capture_has_events_zero_fails():
    # BYPASS-SUITE CASE: "a capture with zero samples" from the task brief.
    ok, detail = lib.capture_has_events_ok(0)
    check(not ok, f"zero events must fail, not vacuously pass ({detail})")


def test_capture_has_events_none_fails():
    # BYPASS-SUITE CASE: an empty/truncated info response (missing field).
    ok, detail = lib.capture_has_events_ok(None)
    check(not ok, f"a missing num_events field must fail, not be treated as fine ({detail})")


def test_capture_has_events_negative_fails():
    ok, detail = lib.capture_has_events_ok(-1)
    check(not ok, f"a negative count must fail ({detail})")


def test_capture_has_events_bool_is_not_a_count():
    # bool is an int subclass in Python -- True must not slip through as "1".
    ok, detail = lib.capture_has_events_ok(True)
    check(not ok, f"a bool is never treated as a real event count ({detail})")


# ── verdict_is_fresh ───────────────────────────────────────────────────────

def test_verdict_fresh_exact_match_ok():
    ok, reason = lib.verdict_is_fresh("1234567890", "1234567890")
    check(ok, f"an exact marker match is fresh ({reason})")


def test_verdict_fresh_missing_run_id_fails():
    # BYPASS-SUITE CASE: the run.id file could not be read at all (e.g. the
    # remote invocation never got far enough to write it).
    ok, reason = lib.verdict_is_fresh(None, "1234567890")
    check(not ok, f"a missing run.id is void evidence, not a pass ({reason})")


def test_verdict_fresh_empty_run_id_fails():
    ok, reason = lib.verdict_is_fresh("", "1234567890")
    check(not ok, f"an empty run.id is void evidence ({reason})")


def test_verdict_fresh_stale_marker_fails():
    # BYPASS-SUITE CASE: a KEEP=1 VM reused across rounds still has a
    # PREVIOUS round's run.id/summary.json on disk; rsync copies it back
    # regardless of whether THIS round's remote command ever ran.
    ok, reason = lib.verdict_is_fresh("1111111111", "2222222222")
    check(not ok, f"a run.id from a previous invocation is stale, never trusted ({reason})")


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


def test_build_demo_summary_no_tabs_ever_reached_is_never_a_pass():
    # BYPASS-SUITE CASE (task brief: "a walk where no tab ever loaded"):
    # every pass has an EMPTY tabs dict. There is nothing to iterate, so
    # `failed` stays empty and (before this floor) `ok` was vacuously True
    # as long as pass_results/extra_checks were merely non-empty containers.
    pass_results = [{"pass": "early", "tabs": {}}, {"pass": "middle", "tabs": {}}]
    extra = {"some_check": {"ok": True}}
    summary = lib.build_demo_summary(pass_results, extra)
    check(not summary["ok"],
          f"zero tabs actually walked must not be a vacuous PASS ({summary})")
    check(summary["failed"] == [], "no individual tab/check failed -- the floor is what catches this")
    check(summary["total_tabs_walked"] == 0, "total_tabs_walked reports the real count")


def test_build_demo_summary_some_tabs_reached_is_fine():
    pass_results = [{"pass": "early", "tabs": {"overview": _tab_result(True)}},
                    {"pass": "middle", "tabs": {}}]
    summary = lib.build_demo_summary(pass_results, {"c": {"ok": True}})
    check(summary["ok"], "at least one tab reached across the passes is enough for this floor")
    check(summary["total_tabs_walked"] == 1, "total_tabs_walked counts across all passes")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"--- {name} ---")
            fn()

    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
