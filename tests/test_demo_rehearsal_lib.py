#!/usr/bin/env python3
"""test_demo_rehearsal_lib.py -- unit tests for tests/demo_rehearsal_lib.py
(issue #157). Pure Python, no browser, no network, no subprocess -- can run
on the Mac (python3 tests/test_demo_rehearsal_lib.py); wired into
tests/unit_tests.list (CI/nightly/box-check) AND, since 2026-09-28
(owner finding, run.id 1790574871), scripts/check.sh directly -- a
regression in the rehearsal's own gating logic must fail `make check`, not
wait for an actual 30-45 minute rehearsal run to surface it.

Usage: python3 tests/test_demo_rehearsal_lib.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo_rehearsal_lib as lib
import ui_live_smoke_lib as ui_lib

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


# ── cpu_clamped_ok / time_model_offcpu_cap_ok ─────────────────────────────
#
# time_model_over_attribution_ok (wait_gap_cpu_ms as a fraction of DB Time)
# was REMOVED (owner correction, 2026-09-27): IO waits are legitimately
# CPU-bearing (the BPF measures on-CPU across the whole wait-start/wait-end
# span, which includes the syscall's own on-CPU work), so that was never a
# valid over-attribution detector -- see cpu_clamped_ok's module-level
# comment in demo_rehearsal_lib.py for the full correction. Only
# cpu_clamped_ok remains, with its narrower (CPU-class-gaps-only) scope.

def test_cpu_clamped_ok_negligible():
    ok, detail = lib.cpu_clamped_ok(cpu_clamped_ms=0.05, db_time_ms=10000.0)
    check(ok, f"negligible cpu_clamped_ms passes ({detail})")


def test_cpu_clamped_fails_over_tolerance():
    ok, detail = lib.cpu_clamped_ok(cpu_clamped_ms=50.0, db_time_ms=10000.0)
    check(not ok, f"cpu_clamped_ms at 0.5% of db_time_ms fails the 0.1% bound ({detail})")


def test_cpu_clamped_ok_states_its_narrow_scope():
    ok, detail = lib.cpu_clamped_ok(cpu_clamped_ms=0.0, db_time_ms=10000.0)
    check(ok, f"zero cpu_clamped_ms passes ({detail})")
    check("wait" in detail.lower(),
          f"detail states the check's narrow scope (CPU-class gaps only) ({detail})")


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
        rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
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
        rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
        offcpu_ms=4000.0, has_measured_cpu=True, used_raw_path=True)
    check(result["conservation"]["ok"],
          f"the identity alone still closes exactly for this dropped-class case ({result})")
    check(not result["ok"],
          f"the combined gate still fails via the Off-CPU* cap ({result})")
    check(not result["offcpu_cap"]["ok"], "offcpu_cap sub-check is the one that failed")


def test_evaluate_window_not_raw_path_fails_loudly():
    rows = [{"name": "CPU*", "ms": 1000.0, "indent": 1}]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=1000.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=False)
    check(not result["ok"],
          "summary compute path fails the window even though the identity closes")
    check(result["compute_path"] == "summary", "compute_path reported accurately")


def test_evaluate_window_zero_db_time_fails_every_subcheck():
    result = lib.evaluate_time_model_window(
        [], db_time_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=True)
    check(not result["ok"], f"a zero-DB-Time window fails the combined gate ({result})")
    check(not result["cpu_clamped"]["ok"],
          "cpu_clamped is not vacuously ok below the DB-Time floor")
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
        rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
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
        rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
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
        rows, db_time_ms=1000.0, cpu_clamped_ms=0.0,
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


# ── bucket_weighted_aas_ok (criteria doc §5, item 2.2) ───────────────────

def test_bucket_weighted_aas_ok_evenly_divisible_window_ok():
    # No truncation: 3 x 1s buckets exactly cover a 3s window, every
    # bucket's total AAS is 2.0 -> weighted derivation is exactly 2.0.
    buckets = [{"t": i * 1_000_000_000, "cpu": 1.5, "lock": 0.5}
               for i in range(3)]
    ok, detail = lib.bucket_weighted_aas_ok(
        buckets, 1_000_000_000, 0, 3_000_000_000, 2.0)
    check(ok, f"evenly-divisible window: weighted derivation matches exactly ({detail})")


def test_bucket_weighted_aas_ok_catches_what_unweighted_mean_would_miss():
    # BYPASS-SUITE CASE -- the whole point of this check. bucket_ns is a
    # CEILING of range_ns/num_buckets (src/compute.c), so a window that
    # does not divide evenly leaves a TRUNCATED tail bucket: here 3 x 1s
    # nominal buckets nominally span 3s, but the true window is only 2.5s
    # -- the server still divides that tail bucket's partial accumulation
    # by the FULL 1s bucket_ns, producing 1.0 instead of what a full 1s
    # would have shown.
    buckets = [
        {"t": 0, "cpu": 1.0, "lock": 1.0},                # 2.0, full bucket
        {"t": 1_000_000_000, "cpu": 1.0, "lock": 1.0},    # 2.0, full bucket
        {"t": 2_000_000_000, "cpu": 0.5, "lock": 0.5},    # 1.0, TRUNCATED tail
    ]
    bucket_ns = 1_000_000_000
    from_ns, to_ns = 0, 2_500_000_000  # true window is 2.5s, not 3s

    # An unweighted mean of the three bucket totals -- the retracted
    # 2026-09-29 walk's approach.
    unweighted_mean = (2.0 + 2.0 + 1.0) / 3  # == 1.6667
    time_model_aas = unweighted_mean  # what time_model "happens to" report
    unweighted_gap_pct = abs(unweighted_mean - time_model_aas) / time_model_aas * 100.0
    check(unweighted_gap_pct <= lib.TIME_MODEL_TOLERANCE_PCT,
          f"setup check: an unweighted mean would PASS this comparison "
          f"(gap={unweighted_gap_pct:.2f}%)")

    ok, detail = lib.bucket_weighted_aas_ok(
        buckets, bucket_ns, from_ns, to_ns, time_model_aas)
    check(not ok, f"the bucket-weighted derivation correctly FAILS the same "
                  f"input an unweighted mean would have passed ({detail})")


def test_bucket_weighted_aas_ok_ignores_cat_and_t_fields():
    # "cat" is a nested per-category breakdown that double-counts against
    # the class total already summed (src/server.c's own comment); "t" is
    # the bucket's start_ns, not an AAS value. Both must be excluded from
    # the per-bucket total, or the derived AAS would be wildly wrong.
    buckets = [{"t": 0, "cpu": 1.0, "lock": 1.0,
               "cat": {"io_worker": 99.0}}]
    ok, detail = lib.bucket_weighted_aas_ok(
        buckets, 1_000_000_000, 0, 1_000_000_000, 2.0)
    check(ok, f"cat/t excluded from the per-bucket total ({detail})")


def test_bucket_weighted_aas_ok_empty_buckets_fails():
    ok, detail = lib.bucket_weighted_aas_ok([], 1_000_000_000, 0, 1_000_000_000, 2.0)
    check(not ok, f"an empty buckets list fails rather than vacuously passing ({detail})")


def test_bucket_weighted_aas_ok_missing_bucket_ns_fails():
    ok, detail = lib.bucket_weighted_aas_ok(
        [{"t": 0, "cpu": 2.0}], None, 0, 1_000_000_000, 2.0)
    check(not ok, f"a missing bucket_ns fails rather than being assumed ({detail})")


def test_bucket_weighted_aas_ok_missing_time_model_aas_fails():
    ok, detail = lib.bucket_weighted_aas_ok(
        [{"t": 0, "cpu": 2.0}], 1_000_000_000, 0, 1_000_000_000, None)
    check(not ok, f"a missing time_model aas fails rather than being skipped ({detail})")


def test_bucket_weighted_aas_ok_non_positive_window_fails():
    ok, detail = lib.bucket_weighted_aas_ok(
        [{"t": 0, "cpu": 2.0}], 1_000_000_000, 1_000_000_000, 1_000_000_000, 2.0)
    check(not ok, f"from_ns == to_ns fails rather than dividing by zero ({detail})")


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


# ── sweep_offset_drift / summarize_sweep_offset_coverage ─────────────────
# (owner finding, 2026-09-28, run.id 1790574871 on 19a95f7 -- numbers below
# are the REAL achieved offsets from that live run, not invented.)

def test_sweep_offset_drift_on_target_zero_drift():
    tick = {"target_offsets_ms": [200, 500, 1000, 1500, 2000],
           "achieved_offsets_ms": [205, 512, 1008, 1503, 2011]}
    d = lib.sweep_offset_drift(tick)
    check(d["first_target_ms"] == 200, "first target reported")
    check(d["first_achieved_ms"] == 205, "first achieved reported")
    check(d["first_drift_ms"] == 5, f"on-target drift is small ({d['first_drift_ms']})")
    check(d["drift_ms"] == [5, 12, 8, 3, 11], f"per-offset drift computed ({d['drift_ms']})")


def test_sweep_offset_drift_late_mount_misses_early_window():
    # BYPASS-SUITE CASE: the real observed drift from the live run -- a
    # 200ms target landing at 669ms. The 200->500ms window (where scatter's
    # 0.1305 and transitions' transients live) was entirely skipped.
    tick = {"target_offsets_ms": [200, 500, 1000, 1500, 2000],
           "achieved_offsets_ms": [669, 985, 1502, 1998, 2503]}
    d = lib.sweep_offset_drift(tick)
    check(d["first_drift_ms"] == 469,
          f"a late mount drifts the first sample far past its target ({d['first_drift_ms']})")
    check(d["first_achieved_ms"] > d["target_offsets_ms"][1],
          "the first achieved sample landed PAST the second offset's own target -- "
          "the 200->500ms window was never actually sampled")


def test_sweep_offset_drift_extreme_observed_case():
    # The most extreme case from the same run: 2416/2446ms against a 200ms
    # target -- the entire sweep compressed into what should have been one
    # offset's worth of time.
    tick = {"target_offsets_ms": [200, 500, 1000, 1500, 2000],
           "achieved_offsets_ms": [2416, 2446, 2480, 2520, 2600]}
    d = lib.sweep_offset_drift(tick)
    check(d["first_drift_ms"] == 2216, f"extreme drift computed correctly ({d['first_drift_ms']})")


def test_sweep_offset_drift_mismatched_lengths_does_not_crash():
    # BYPASS-SUITE CASE: a truncated sweep (fewer achieved samples than
    # targets, e.g. a mid-sweep capture failure) must not crash the report.
    tick = {"target_offsets_ms": [200, 500, 1000, 1500, 2000],
           "achieved_offsets_ms": [669, 985]}
    d = lib.sweep_offset_drift(tick)
    check(len(d["drift_ms"]) == 2, f"drift only computed for the offsets that were actually achieved ({d})")


def test_sweep_offset_drift_empty_tick_does_not_crash():
    d = lib.sweep_offset_drift({})
    check(d["drift_ms"] == [], "an empty/missing tick record reports no drift, not a crash")
    check(d["first_drift_ms"] is None, "first_drift_ms is None, not a fabricated 0")


def test_sweep_offset_drift_carries_capture_ms_and_panel_dims():
    # issue #252 secondary finding: build_sweep_tick_record already measured
    # capture_ms/capture_ms_total_ms/panel_dims, but this function -- the
    # one sweep_offset_coverage (the diagnostic array a human actually reads
    # for drift) is built from -- silently dropped all three when
    # re-deriving its own per-tick dict. A reader following the drift array
    # alone (as #252's own evidence section did) saw no cost data at all.
    dims = {"box_width": 1698.0, "box_height": 2340.0,
            "clip_width": 1698.0, "clip_height": 700.0}
    tick = {"target_offsets_ms": [200, 500], "achieved_offsets_ms": [205, 512],
           "capture_ms": [210.5, 198.2], "capture_ms_total_ms": 408.7,
           "panel_dims": dims}
    d = lib.sweep_offset_drift(tick)
    check(d["capture_ms"] == [210.5, 198.2],
          f"per-frame capture cost is visible next to the drift it may "
          f"explain, not dropped ({d.get('capture_ms')})")
    check(d["capture_ms_total_ms"] == 408.7,
          f"the tick's total capture cost is carried through ({d.get('capture_ms_total_ms')})")
    check(d["panel_dims"] == dims,
          f"the panel size actually captured is carried through ({d.get('panel_dims')})")


def test_sweep_offset_drift_capture_ms_absent_defaults_empty():
    tick = {"target_offsets_ms": [200], "achieved_offsets_ms": [205]}
    d = lib.sweep_offset_drift(tick)
    check(d["capture_ms"] == [],
          "a tick record that never measured capture cost gets [], not a missing key")
    check(d["capture_ms_total_ms"] is None,
          "capture_ms_total_ms is None (not fabricated 0.0) when the source tick never had it")
    check(d["panel_dims"] == {},
          "a tick record that never measured panel dims gets {}, not a missing key")


def test_summarize_sweep_offset_coverage_joins_tab_and_pass():
    ticks = [
        {"target_offsets_ms": [200, 500], "achieved_offsets_ms": [205, 510]},
        {"target_offsets_ms": [200, 500], "achieved_offsets_ms": [669, 985]},
    ]
    summary = lib.summarize_sweep_offset_coverage("scatter", "early", ticks)
    check(summary["tab"] == "scatter" and summary["pass"] == "early",
          "tab/pass identity carried through")
    check(len(summary["ticks"]) == 2, "one drift record per input tick")
    check(summary["ticks"][1]["first_drift_ms"] == 469,
          f"the late-mount tick's drift is visible in the summary ({summary['ticks'][1]})")


def test_summarize_sweep_offset_coverage_empty_ticks():
    summary = lib.summarize_sweep_offset_coverage("overview", "early", [])
    check(summary["ticks"] == [], "no ticks -> empty list, not a crash")
    summary_none = lib.summarize_sweep_offset_coverage("overview", "early", None)
    check(summary_none["ticks"] == [], "None ticks -> empty list, not a crash")


# ── daemon_integrity_ok (criteria doc §6) ─────────────────────────────────

def test_daemon_integrity_all_zero_ok():
    metrics = {"ringbuf_drops_total": 0, "state_map_full_total": 0,
              "seen_query_ids_full_total": 0, "some_other_field": 123}
    ok, detail = lib.daemon_integrity_ok(metrics)
    check(ok, f"all three counters at zero passes ({detail})")


def test_daemon_integrity_ringbuf_drops_fails():
    # BYPASS-SUITE CASE: trace events were dropped -- DB Time is built from
    # trace events, so this is upstream of every other check in this file.
    metrics = {"ringbuf_drops_total": 5, "state_map_full_total": 0,
              "seen_query_ids_full_total": 0}
    ok, detail = lib.daemon_integrity_ok(metrics)
    check(not ok, f"nonzero ringbuf_drops_total fails ({detail})")
    check("ringbuf_drops_total" in detail, f"detail names the offending counter ({detail})")


def test_daemon_integrity_state_map_full_fails():
    metrics = {"ringbuf_drops_total": 0, "state_map_full_total": 2,
              "seen_query_ids_full_total": 0}
    ok, detail = lib.daemon_integrity_ok(metrics)
    check(not ok, f"nonzero state_map_full_total fails ({detail})")


def test_daemon_integrity_seen_query_ids_full_fails():
    metrics = {"ringbuf_drops_total": 0, "state_map_full_total": 0,
              "seen_query_ids_full_total": 1}
    ok, detail = lib.daemon_integrity_ok(metrics)
    check(not ok, f"nonzero seen_query_ids_full_total fails ({detail})")


def test_daemon_integrity_missing_counter_fails():
    # BYPASS-SUITE CASE: an empty/truncated metrics response -- a gate that
    # cannot see the count must refuse, not assume it is fine.
    metrics = {"ringbuf_drops_total": 0}
    ok, detail = lib.daemon_integrity_ok(metrics)
    check(not ok, f"a missing counter fails rather than being treated as zero ({detail})")


def test_daemon_integrity_non_dict_response_fails():
    # BYPASS-SUITE CASE: the control/metrics query itself failed (e.g.
    # {"error": "daemon not running"}) and the caller passed the whole
    # error response through.
    ok, detail = lib.daemon_integrity_ok(None)
    check(not ok, f"a non-dict metrics response fails outright ({detail})")


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


# ── tab_coverage_check_ok (issue #214 review: wire the coverage checker
# into the rehearsal's own gating verdict) ────────────────────────────────

def test_tab_coverage_check_ok_all_pass():
    results = {"overview": {"ok": True, "detail": "x"},
               "waterfall": {"ok": True, "detail": "y"}}
    ok, detail = lib.tab_coverage_check_ok(results)
    check(ok, f"all tabs ok -> the aggregate check passes ({detail})")


def test_tab_coverage_check_ok_one_tab_fails_fails_the_whole_check():
    """RED: this is the exact case #214 exists for -- a single empty tab
    (Waterfall showing 'No executions for selected range') must turn the
    rehearsal's OWN verdict false, not just print a line nobody reads."""
    results = {"overview": {"ok": True, "detail": "x"},
               "waterfall": {"ok": False, "detail": "no slow execution"}}
    ok, detail = lib.tab_coverage_check_ok(results)
    check(not ok, f"one failing tab FAILS the aggregate check ({detail})")
    check("waterfall" in detail, f"the failing tab is named ({detail})")


def test_tab_coverage_check_ok_empty_results_fails():
    """A gate that received nothing to check (the coverage run never
    happened, or ran against the wrong window) must refuse, not vacuously
    pass on zero tabs checked."""
    ok, detail = lib.tab_coverage_check_ok({})
    check(not ok, f"empty results dict FAILS, not a vacuous PASS ({detail})")
    ok, detail = lib.tab_coverage_check_ok(None)
    check(not ok, f"None results FAILS, does not crash ({detail})")


def test_tab_coverage_check_ok_wrong_tab_set_fails():
    """A coverage run that silently checked a DIFFERENT set of tabs than
    expected (a wiring bug, e.g. an outdated TAB_ORDER import) must fail
    loudly rather than reporting ok on whatever partial set it happened to
    see."""
    results = {"overview": {"ok": True}, "events": {"ok": True}}
    ok, detail = lib.tab_coverage_check_ok(
        results, expected_tabs=("overview", "events", "waterfall"))
    check(not ok, f"missing an expected tab FAILS ({detail})")
    check("waterfall" in detail, f"the missing tab is named ({detail})")


def test_tab_coverage_check_ok_matching_expected_tabs_passes():
    results = {"overview": {"ok": True}, "events": {"ok": True}}
    ok, detail = lib.tab_coverage_check_ok(
        results, expected_tabs=("overview", "events"))
    check(ok, f"exact expected tab set, all ok, passes ({detail})")


def test_tab_coverage_check_ok_could_not_evaluate_fails_but_named_separately():
    """RED (round 3 review): a window_too_large refusal on a demo-length
    capture must still fail the check (fail-closed), but must NOT read as
    "not populated" in the detail -- conflating the two sends someone
    chasing an empty tab that was never empty."""
    results = {"overview": {"ok": True, "detail": "x"},
               "waterfall": {"ok": False, "could_not_evaluate": True,
                              "detail": "executions COULD NOT EVALUATE"}}
    ok, detail = lib.tab_coverage_check_ok(results)
    check(not ok, f"a could_not_evaluate tab still FAILS the check ({detail})")
    check("waterfall" in detail, f"the tab is named ({detail})")
    check("not populated" not in detail,
          f"a could_not_evaluate tab is NOT described as 'not populated' ({detail})")
    check("COULD NOT EVALUATE" in detail or "could not evaluate" in detail.lower(),
          f"the detail says COULD NOT EVALUATE, distinct wording ({detail})")


def test_tab_coverage_check_ok_separates_both_buckets_when_both_present():
    results = {
        "overview": {"ok": False, "detail": "empty"},
        "waterfall": {"ok": False, "could_not_evaluate": True,
                       "detail": "window_too_large"},
    }
    ok, detail = lib.tab_coverage_check_ok(results)
    check(not ok, f"either bucket alone fails the check ({detail})")
    check("overview" in detail and "waterfall" in detail,
          f"both tabs are named, in their own bucket ({detail})")


def test_tab_coverage_check_ok_could_not_evaluate_without_key_defaults_to_not_populated():
    """A result dict with no could_not_evaluate key at all (every checker
    that does not hit COULD_NOT_EVALUATE_CODES) must default to the
    ordinary "not populated" bucket, not silently vanish from both."""
    results = {"waterfall": {"ok": False, "detail": "empty, no key at all"}}
    ok, detail = lib.tab_coverage_check_ok(results)
    check(not ok, f"missing key still FAILS ({detail})")
    check("not populated" in detail,
          f"defaults to 'not populated' when could_not_evaluate is absent ({detail})")


# ── the #214 wiring into demo_rehearsal.py itself still exists (round 3
# review item 2) ────────────────────────────────────────────────────────

def test_demo_rehearsal_still_wires_tab_coverage_into_extra_checks():
    """Guards against a future edit silently deleting the coverage-check
    block from demo_rehearsal.py: build_demo_summary() gates on whatever
    keys happen to be in extra_checks, so a dropped block would silently
    become "nothing to check here, all green" -- the exact failure #214
    exists to prevent, one level up, where nothing else would catch it
    (the wrong-tab-set guard above catches a DIFFERENT tab set, not a
    MISSING block entirely).

    Source-inspection, not an import: demo_rehearsal.py imports Playwright
    at module scope, and this test file is in tests/unit_tests.list (CI's
    build-and-unit job has no Playwright) -- importing the driver here
    would reproduce the exact #205 bug this repo already fixed once on a
    sibling file (see test_import_needs_no_playwright in
    tests/test_demo_workload_coverage.py)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "demo_rehearsal.py")
    with open(path) as f:
        src = f.read()
    check('extra_checks["tab_coverage"]' in src,
          "demo_rehearsal.py still assigns extra_checks['tab_coverage']")
    check("cov.run_coverage(" in src,
          "demo_rehearsal.py still calls cov.run_coverage(...)")
    check("drlib.tab_coverage_check_ok(" in src,
          "demo_rehearsal.py still calls drlib.tab_coverage_check_ok(...)")
    check("import demo_workload_coverage as cov" in src,
          "demo_rehearsal.py still imports demo_workload_coverage")


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


# ── Blink-gate regression: demo_rehearsal's dependency on
# ui_live_smoke_lib.build_tab_result's SWEEP-BASED verdict (criteria doc /
# owner finding, 2026-09-28, run.id 1790574871 on master 19a95f7) ─────────
#
# demo_rehearsal.py has NO independent screenshot/blink-measurement code of
# its own -- every tab's verdict comes straight from
# ui_live_smoke.py:run_tab(), which calls ui_live_smoke_lib's own
# blink_sweep_gate_verdict (the worst consecutive-pair ratio across the
# WHOLE mount-anchored offset sweep, #209) to compute blink_ratio, then
# build_tab_result(blink_ratio=...) to grade the tab. This IS that
# dependency, pinned with the REAL numbers a live rehearsal measured
# (run.id 1790574871): the OLD single-anchored-pair gate reported ratio 0.0
# for scatter and transitions (PASS) because its one sampled pair landed
# AFTER their real transients at the 200->500ms sweep pair; the fix grades
# on the WORST ratio across the whole sweep instead. A future change that
# reverts demo_rehearsal.py to computing its own single-pair ratio, or that
# changes build_tab_result's grading formula, fails HERE -- in `make
# check` -- rather than silently only on a 30-45 minute rehearsal.
_BLINK_GATE_COMMON_KWARGS = dict(
    rendered_ok=True, rendered_detail="ok", ticks_observed=6,
    console_errors=[], color_violations=[],
    leak_before={"pending": 0}, leak_after={"pending": 0}, artifacts={},
)


def test_blink_gate_scatter_13pct_transient_fails_run_1790574871():
    # scatter, run.id 1790574871: tick 3's worst-sweep-pair ratio (the
    # 200->500ms pair) measured 0.1305; ticks 1/2/4/5/6 assumed clean (0.0)
    # -- the minimal fixture needed to prove max() over the sweep catches
    # it (this is the same case demonstrated red/green in this task's
    # standalone proof before landing here).
    sweep_ratios = [0.0, 0.0, 0.1305, 0.0, 0.0, 0.0]
    blink_ratio = max(sweep_ratios)
    result = ui_lib.build_tab_result(
        tab_id="scatter", blink_ratio=blink_ratio,
        blink_pair_offsets_ms=[100] * len(sweep_ratios), blink_not_measured=[],
        **_BLINK_GATE_COMMON_KWARGS)
    check(result["no_blink"]["ratio"] == 0.1305,
          f"the tab's graded ratio is the sweep's WORST pair, not an anchored single pair ({result['no_blink']['ratio']})")
    check(not result["no_blink"]["ok"],
          f"0.1305 fails the {ui_lib.BLINK_THRESHOLD} blink threshold ({result['no_blink']})")
    check(not result["ok"],
          f"scatter's tab-level verdict is FAIL, reproducing run.id 1790574871's finding ({result['ok']})")


def test_blink_gate_transitions_subpercent_transient_fails_run_1790574871():
    # transitions, run.id 1790574871: ticks 3-6's worst-sweep-pair ratios
    # (also the 200->500ms pair) measured 0.0172, 0.0097, 0.0150, 0.0072.
    sweep_ratios = [0.0, 0.0, 0.0172, 0.0097, 0.0150, 0.0072]
    blink_ratio = max(sweep_ratios)
    result = ui_lib.build_tab_result(
        tab_id="transitions", blink_ratio=blink_ratio,
        blink_pair_offsets_ms=[100] * len(sweep_ratios), blink_not_measured=[],
        **_BLINK_GATE_COMMON_KWARGS)
    check(abs(result["no_blink"]["ratio"] - 0.0172) < 1e-9,
          f"graded ratio is tick 3's 0.0172, the worst of the four ({result['no_blink']['ratio']})")
    check(not result["ok"],
          f"transitions' tab-level verdict is FAIL, reproducing run.id 1790574871's finding ({result['ok']})")


def test_blink_gate_queries_2_6pct_transient_still_fails_run_1790574871():
    # queries, run.id 1790574871: the OLD anchored-pair gate ALREADY caught
    # this one (tick 2 = 0.0262, at the 500->1000ms pair the old anchor
    # happened to land on) -- included as a consistency check that the NEW
    # sweep-based gate still fails it too, not just tabs the old gate
    # missed.
    sweep_ratios = [0.0, 0.0262, 0.0, 0.0, 0.0, 0.0]
    blink_ratio = max(sweep_ratios)
    result = ui_lib.build_tab_result(
        tab_id="queries", blink_ratio=blink_ratio,
        blink_pair_offsets_ms=[100] * len(sweep_ratios), blink_not_measured=[],
        **_BLINK_GATE_COMMON_KWARGS)
    check(not result["ok"],
          f"queries' tab-level verdict stays FAIL under the new gate too ({result['ok']})")


def test_blink_gate_clean_trace_still_passes():
    # Baseline: a genuinely clean sweep (every tick's worst pair ~0) must
    # still pass -- this suite only closes the false-PASS hole, it must
    # never introduce a false FAIL on a clean trace.
    sweep_ratios = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    blink_ratio = max(sweep_ratios)
    result = ui_lib.build_tab_result(
        tab_id="overview", blink_ratio=blink_ratio,
        blink_pair_offsets_ms=[100] * len(sweep_ratios), blink_not_measured=[],
        **_BLINK_GATE_COMMON_KWARGS)
    check(result["ok"], f"a genuinely clean sweep still passes ({result})")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"--- {name} ---")
            fn()

    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
