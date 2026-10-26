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
import re
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


# ── cpu_clamped_ok / time_model_offcpu_diagnostic ─────────────────────────
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


def test_offcpu_diagnostic_reports_physical_magnitude():
    result = lib.time_model_offcpu_diagnostic(
        offcpu_ms=400.0, db_time_ms=10000.0, has_measured_cpu=True)
    check(result["available"], "exact CPU data supplies a physical residual")
    check(result["offcpu_ms"] == 400.0 and result["pct"] == 4.0,
          f"physical ms and DB-Time percentage remain reported ({result})")
    check(result["ok"] is None and result["gating"] is False,
          "a diagnostic has no pass/fail verdict and explicitly does not gate")


def test_offcpu_diagnostic_large_residual_is_reported_not_gated():
    result = lib.time_model_offcpu_diagnostic(
        offcpu_ms=4000.0, db_time_ms=10000.0, has_measured_cpu=True)
    check(result["pct"] == 40.0, f"even a large residual stays visible ({result})")
    check(result["ok"] is None and not result["gating"],
          "large physical Off-CPU has no attribution-correctness verdict")
    check("reported, not gated" in result["detail"],
          "the reporting-only decision is explicit in the human-readable detail")


def test_offcpu_diagnostic_unavailable_without_measured_cpu():
    result = lib.time_model_offcpu_diagnostic(
        offcpu_ms=9999.0, db_time_ms=10000.0, has_measured_cpu=False)
    check(not result["available"] and result["ok"] is None,
          "legacy/sampled or summary data is unavailable, never a passing reading")
    check(result["offcpu_ms"] is None and result["pct"] is None,
          "unavailable Off-CPU is not reported as a measured zero")


# #387 x the reporting-only diagnostic (merge of agent/cx-387-absence into
# the branch that replaced time_model_offcpu_cap_ok with
# time_model_offcpu_diagnostic). src/compute.c now only sets
# has_measured_cpu when a record has a NON-ZERO in-window fraction, so an
# empty window -- records selected by end timestamp but with no overlap --
# arrives here as has_measured_cpu=False while every scalar is 0.0. The two
# changes have to compose into THREE distinguishable readings, because a
# scalar 0.0 is a legitimate measurement in one of them:
#   absence       has_measured_cpu=False -> available False, pct None
#   no denominator exact data, db_time below the floor -> available False,
#                 pct None, but a DIFFERENT reason string
#   checked clean exact data, real db_time, 0.0 residual -> available True,
#                 pct 0.0 (a measured zero, not None)
# Asserted with `is None` / `== 0.0` rather than truthiness: `not pct`
# holds for both None and 0.0, which is exactly the conflation #387 exists
# to prevent.
def test_offcpu_diagnostic_empty_window_absence_vs_measured_zero():
    absent = lib.time_model_offcpu_diagnostic(
        offcpu_ms=0.0, db_time_ms=0.0, has_measured_cpu=False)
    no_denom = lib.time_model_offcpu_diagnostic(
        offcpu_ms=0.0, db_time_ms=0.0, has_measured_cpu=True)
    clean = lib.time_model_offcpu_diagnostic(
        offcpu_ms=0.0, db_time_ms=10000.0, has_measured_cpu=True)
    check(absent["available"] is False and absent["pct"] is None
          and absent["offcpu_ms"] is None,
          f"an empty window reports absence, not a measured zero ({absent})")
    check(absent["ok"] is None and absent["gating"] is False,
          f"absence is not a verdict either way ({absent})")
    check("unavailable, not zero" in absent["detail"],
          f"the absence reason is spelled out for a reader ({absent['detail']})")
    check(no_denom["available"] is False and no_denom["pct"] is None
          and no_denom["detail"] != absent["detail"],
          f"exact data with no DB Time is its own reason, not the absence one "
          f"({no_denom['detail']})")
    check(clean["available"] is True and clean["pct"] == 0.0
          and clean["offcpu_ms"] == 0.0,
          f"checked-and-clean keeps its measured zero ({clean})")
    check(clean["detail"] != absent["detail"]
          and clean["available"] != absent["available"],
          f"'checked and clean' is distinguishable from 'not applicable' "
          f"({clean['detail']} vs {absent['detail']})")


def test_evaluate_window_empty_window_offcpu_is_absent_not_clean():
    """The same combination through the full per-window evaluator: an empty
    window must not produce an `offcpu` block that reads as a measured,
    clean 0%. Conservation's own nonzero-DB-Time floor is what fails the
    window; the diagnostic's job is only to stay honest about absence."""
    result = lib.evaluate_time_model_window(
        [], db_time_ms=0.0, cpu_clamped_ms=0.0, offcpu_ms=0.0,
        has_measured_cpu=False, used_raw_path=True)
    off = result["offcpu"]
    check(off["available"] is False and off["pct"] is None,
          f"empty window's offcpu block reports absence ({off})")
    check(off["ok"] is None and off["gating"] is False,
          f"the diagnostic stays outside the verdict ({off})")
    check(not result["ok"],
          f"the window still fails -- on the floor, not on Off-CPU ({result})")
    check("offcpu_cap" not in result,
          f"no resurrected cap sub-check ({sorted(result)})")


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


def test_evaluate_window_dropped_wait_class_is_not_detected_by_residual():
    # An entire wait class (Lock, ~3000ms) is dropped from rows; the residual
    # definition means its time is absorbed into Off-CPU* and the identity
    # still closes EXACTLY. The identity sub-check alone says ok=True; the
    # combined accounting check cannot detect this either. Keep this blind
    # spot explicit; per-class attribution evidence is a separate scan.
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
    check(result["ok"],
          f"accounting closes despite the dropped label; no correctness cap is inferred ({result})")
    check(result["offcpu"]["pct"] == 40.0 and result["offcpu"]["ok"] is None,
          "the raised residual is visible as diagnostic data, not an attribution verdict")


def test_evaluate_window_clean_burst_phases_do_not_fail_conservation():
    # #386's 1775-placement clean-run distribution: min, median, p95, max.
    samples = []
    for offset, pct in enumerate((5.08, 8.96, 11.25, 13.52)):
        offcpu_ms = pct * 100.0
        rows = [{"name": "CPU (running)", "ms": 10000.0 - offcpu_ms, "indent": 1},
                {"name": "CPU (waiting for a core)", "ms": offcpu_ms, "indent": 1}]
        result = lib.evaluate_time_model_window(
            rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
            offcpu_ms=offcpu_ms, has_measured_cpu=True, used_raw_path=True)
        check(result["ok"], f"clean burst phase at {pct}% does not fail accounting")
        check(round(result["offcpu"]["pct"], 2) == pct,
              "each sampled physical percentage is retained at the measured precision")
        samples.append(dict(result, offset_s=offset * 300.0))
    aggregate = lib.build_demo_conservation_check(samples, {"ok": True})
    summary = lib.build_demo_summary(
        [{"pass": "early", "tabs": {"overview": {"ok": True}}}],
        {"time_model_conserves": aggregate})
    check(summary["ok"], "non-gating diagnostics cannot fail the final rehearsal verdict")
    check(aggregate["samples"][-1]["offcpu"]["pct"] == 13.52,
          "the high phase remains auditable in the saved conservation samples")


def test_evaluate_window_cpu_clamp_still_fails_with_large_offcpu():
    rows = [{"name": "CPU (running)", "ms": 6000.0, "indent": 1},
            {"name": "CPU (waiting for a core)", "ms": 4000.0, "indent": 1}]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=10000.0, cpu_clamped_ms=50.0,
        offcpu_ms=4000.0, has_measured_cpu=True, used_raw_path=True)
    check(not result["ok"] and not result["cpu_clamped"]["ok"],
          "reporting Off-CPU does not weaken the independent CPU-clamp gate")


def test_evaluate_window_not_raw_path_fails_loudly():
    rows = [{"name": "CPU*", "ms": 1000.0, "indent": 1}]
    result = lib.evaluate_time_model_window(
        rows, db_time_ms=1000.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=False)
    check(not result["ok"],
          "summary compute path fails the window even though the identity closes")
    check(result["compute_path"] == "summary", "compute_path reported accurately")


def test_evaluate_window_zero_db_time_fails_and_offcpu_is_unavailable():
    result = lib.evaluate_time_model_window(
        [], db_time_ms=0.0, cpu_clamped_ms=0.0,
        offcpu_ms=0.0, has_measured_cpu=False, used_raw_path=True)
    check(not result["ok"], f"a zero-DB-Time window fails the combined gate ({result})")
    check(not result["cpu_clamped"]["ok"],
          "cpu_clamped is not vacuously ok below the DB-Time floor")
    check(result["offcpu"]["ok"] is None and not result["offcpu"]["available"],
          "Off-CPU is explicitly unavailable below the DB-Time floor")
    check(result["offcpu"]["pct"] is None,
          "a void window is not reported as measured 0% Off-CPU")


def test_offcpu_diagnostic_void_measured_window_has_no_percentage():
    result = lib.time_model_offcpu_diagnostic(
        offcpu_ms=0.2, db_time_ms=0.5, has_measured_cpu=True)
    check(result["offcpu_ms"] == 0.2 and result["pct"] is None,
          "retain the measured residual but do not divide by a void DB-Time reading")
    check(not result["available"] and result["ok"] is None,
          "a near-empty measured window has no usable Off-CPU percentage")


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
        {"offset_s": 0.0, "ok": True, "offcpu": {"available": True}},
        {"offset_s": 300.0, "ok": False, "offcpu": {"available": True}},
        {"offset_s": 600.0, "ok": True, "offcpu": {"available": True}},
    ]
    result = lib.build_demo_conservation_check(samples, {"ok": True})
    check(not result["ok"],
          "one failing sample fails the whole check -- not an average, not just the last one")
    check(result["failed_offsets_s"] == [300.0],
          f"the failing offset is named (got {result['failed_offsets_s']})")


def test_conservation_samples_all_pass():
    samples = [{"offset_s": 0.0, "ok": True, "offcpu": {"available": True}}, {"offset_s": 300.0, "ok": True, "offcpu": {"available": True}}]
    result = lib.build_demo_conservation_check(samples, {"ok": True})
    check(result["ok"], "every sample passing -> ok")
    check(result["num_samples"] == 2, "num_samples reported")


# ── workload_signature_present_ok / aas_floor_ok (criteria doc §2) ───────

def test_workload_signature_both_present_ok():
    rows = [
        {"name": "Lock:relation", "ms": 300.0, "indent": 2},
        {"name": "Lock:advisory", "ms": 200.0, "indent": 2},
    ]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(ok, f"both signature events present with real time ({detail})")


def test_workload_signature_missing_one_fails():
    # BYPASS-SUITE CASE (criteria doc §2): we captured something other than
    # the intended workload -- Lock:advisory never shows up at all.
    rows = [{"name": "Lock:relation", "ms": 300.0, "indent": 2}]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(not ok, f"a missing required event fails ({detail})")
    check("Lock:advisory" in detail, f"detail names what's missing ({detail})")


def test_workload_signature_zero_time_fails():
    # Present in name only, zero real time -- must not count as "present".
    rows = [
        {"name": "Lock:relation", "ms": 0.0, "indent": 2},
        {"name": "Lock:advisory", "ms": 150.0, "indent": 2},
    ]
    ok, detail = lib.workload_signature_present_ok(rows)
    check(not ok, f"a zero-time row does not count as present ({detail})")


def test_workload_signature_empty_rows_fails():
    ok, detail = lib.workload_signature_present_ok([])
    check(not ok, f"empty rows (e.g. an idle/empty window) fails ({detail})")


def test_aas_floor_above_withdrawn_floor_is_reported_only():
    ok, detail = lib.aas_floor_ok(1.2)
    check(ok is None, f"aas=1.2 has no pass/fail verdict ({detail})")
    check("aas=1.2" in detail and "reported, not gated" in detail,
          f"above-floor AAS and its diagnostic status remain visible ({detail})")


def test_aas_floor_below_withdrawn_floor_is_reported_only():
    # Anti-vacuity belongs to the independent DB-Time/events/signature gates.
    ok, detail = lib.aas_floor_ok(0.1)
    check(ok is None, f"aas=0.1 has no pass/fail verdict ({detail})")
    check("aas=0.1" in detail and "reported, not gated" in detail,
          f"near-zero AAS is retained as diagnostic data ({detail})")


def test_aas_floor_none_is_reported_only():
    ok, detail = lib.aas_floor_ok(None)
    check(ok is None, f"missing AAS has no pass/fail verdict ({detail})")
    check("aas=None" in detail and "reported, not gated" in detail,
          f"missing AAS stays visibly absent, not a measured zero ({detail})")


def test_aas_floor_between_old_floors_is_reported_only():
    # Pin withdrawal of both 0.5 and 1.15, rather than a relaxed threshold.
    ok, detail = lib.aas_floor_ok(0.8)
    check(ok is None, f"aas=0.8 has no pass/fail verdict ({detail})")
    check("aas=0.8" in detail and "reported, not gated" in detail,
          f"between-floor AAS remains diagnostic ({detail})")


# ── evaluate_time_model_window with the criteria-doc #2 extra floors ─────

def test_evaluate_window_workload_signature_and_aas_when_requested():
    rows = [
        {"name": "CPU (running)", "ms": 6000.0, "indent": 1},
        {"name": "Lock", "ms": 3600.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 400.0, "indent": 1},
        {"name": "Lock:relation", "ms": 3000.0, "indent": 2},
        {"name": "Lock:advisory", "ms": 500.0, "indent": 2},
    ]
    samples = []
    for aas in (None, 0.0, 0.1, 0.8, 1.01, 1.12, 1.15, 1.30, 1.5):
        result = lib.evaluate_time_model_window(
            rows, db_time_ms=10000.0, cpu_clamped_ms=0.0,
            offcpu_ms=400.0, has_measured_cpu=True, used_raw_path=True,
            aas=aas, check_workload_signature=True, check_aas_floor=True)
        check(result["ok"], f"independent gates pass regardless of AAS ({aas!r})")
        check(result["workload_signature"]["ok"], "workload_signature sub-check recorded ok")
        diagnostic = result["aas_floor"]
        check(diagnostic["ok"] is None and diagnostic.get("gating") is False,
              "AAS uses the Off-CPU reported-only verdict shape")
        check("aas" in diagnostic and diagnostic["aas"] == aas,
              f"the original AAS value is retained ({aas!r})")
        check(f"aas={aas!r}" in diagnostic["detail"]
              and "reported, not gated" in diagnostic["detail"],
              "AAS detail retains the value and explains its diagnostic status")
        samples.append(dict(result, offset_s=len(samples) * 300.0))
    aggregate = lib.build_demo_conservation_check(samples, {"ok": True})
    summary = lib.build_demo_summary(
        [{"pass": "early", "tabs": {"overview": {"ok": True}}}],
        {"time_model_conserves": aggregate})
    check(summary["ok"], "reported-only AAS cannot fail the final rehearsal verdict")
    check(summary["checks"]["time_model_conserves"]["samples"][4]["aas_floor"].get("aas") == 1.01,
          "the final summary preserves gate-2-range AAS diagnostics")


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


# ── Historical DB-Time diagnostic (non-gating since #258) ───────────────

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


# -- class_rows_vs_top_events_ok (issue #517) ----------------------------
#
# Both fixtures below are the REAL responses the real pgwt-server returned
# for one synthetic trace on the raw/exact path (measured on this Mac
# 2026-10-25 -- a 60s window over a trace carrying CPU, Lock, LWLock, IO,
# Client:ClientRead, a Timeout pacing event and an Activity event, with
# cpu_measured=1). Numbers are verbatim, including the 659.9999/1320.0001
# split, so the tests below exercise the shapes the product actually emits:
# idle-but-visible rows carrying `pct: null`, the two-row CPU group, and
# indent-2 sub-event rows under each class.

def _real_tm_rows():
    return [
        {"name": "DB Time", "ms": 7980.0, "indent": 0},
        {"name": "IO", "ms": 2000.0, "indent": 1},
        {"name": "IO:AioIoUringExecution", "ms": 2000.0, "indent": 2},
        {"name": "Lock", "ms": 2000.0, "indent": 1},
        {"name": "Lock:extend", "ms": 2000.0, "indent": 2},
        {"name": "LWLock", "ms": 2000.0, "indent": 1},
        {"name": "LWLock:ShmemIndex", "ms": 2000.0, "indent": 2},
        {"name": "CPU (running)", "ms": 659.9999, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 1320.0001, "indent": 1},
        {"name": "Idle", "ms": 6000.0, "indent": 0},
        {"name": "Client:ClientRead", "ms": 2000.0, "indent": 2},
        {"name": "Timeout:CheckpointWriteDelay", "ms": 2000.0, "indent": 2},
        {"name": "Other idle (parked background loops)", "ms": 2000.0, "indent": 2},
    ]


def _real_te_rows():
    return [
        {"class": "LWLock", "name": "LWLock:ShmemIndex",
         "total_ms": 2000.0, "pct": 25.062656641604008},
        {"class": "Lock", "name": "Lock:extend",
         "total_ms": 2000.0, "pct": 25.062656641604008},
        {"class": "Client", "name": "Client:ClientRead",
         "total_ms": 2000.0, "pct": None},
        {"class": "Timeout", "name": "Timeout:CheckpointWriteDelay",
         "total_ms": 2000.0, "pct": None},
        {"class": "IO", "name": "IO:AioIoUringExecution",
         "total_ms": 2000.0, "pct": 25.062656641604008},
        {"class": "CPU", "name": "CPU*", "total_ms": 1980.0,
         "pct": 24.81203007518797},
    ]


REAL_TE_DB_TIME_MS = 7980.0


def test_class_attribution_real_raw_path_response_agrees():
    ok, detail = lib.class_rows_vs_top_events_ok(
        _real_tm_rows(), _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(ok, f"the real raw-path responses agree per class ({detail})")
    check("worst gap 0.00%" in detail,
          f"and agree exactly, not merely inside tolerance (got: {detail})")
    # The CPU group is two rows in time_model and one in top_events; the
    # fold has to be exact or this check would be red on every healthy run.
    check("CPU tm=1980.0/te=1980.0" in detail,
          f"CPU (running)+CPU (waiting for a core) folds onto top_events' "
          f"single CPU row (got: {detail})")
    # Idle-but-visible rows must not enter either side.
    check("Client" not in detail and "Timeout" not in detail,
          f"idle-but-visible rows (pct: null) are excluded from both sides "
          f"(got: {detail})")


def test_class_attribution_catches_the_dropped_class_the_identity_absorbs():
    # THE POINT OF THIS CHECK, and issue #517's demonstrated red.
    #
    # Mutation: Lock's 2000 ms is never attributed to its own class row.
    # This is not an arbitrary edit -- it is exactly what src/compute.c does
    # with unattributed wait time: db_time_ns is unchanged (it is summed
    # before any class attribution), the Lock class row drops out (a class
    # with total_ns <= 0 is not emitted at all), and Off-CPU* -- defined as
    # `db_time - CPU* - Sigma waits` -- grows by the same 2000 ms.
    rows = [r for r in _real_tm_rows() if r["name"] != "Lock"]
    for r in rows:
        if r["name"] == "CPU (waiting for a core)":
            r["ms"] += 2000.0

    # Leg 1: the identity still closes to EXACTLY zero. This is the
    # false negative #517 was filed about, asserted rather than described.
    cons_ok, cons_detail = lib.time_model_conservation(rows, 7980.0)
    check(cons_ok,
          f"time_model_conservation still PASSES the dropped class "
          f"({cons_detail})")
    check("gap=0.0ms" in cons_detail,
          f"and with a gap of exactly 0.0 ms, not a shrunken one "
          f"(got: {cons_detail})")

    # Leg 2: the independently derived comparison goes red, and names the
    # class, because top_events still carries Lock:extend's 2000 ms.
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(not ok, f"class_rows_vs_top_events_ok FAILS the same input "
                  f"({detail})")
    check("Lock: time_model=0.0ms vs top_events=2000.0ms" in detail,
          f"and names the dropped class and both numbers (got: {detail})")
    check("CPU: time_model=3980.0ms vs top_events=1980.0ms" in detail,
          f"and names the row the time landed in instead (got: {detail})")


def test_class_attribution_subsumes_total_check_for_one_dropped_event():
    # Captured from the production raw accumulators on d02b7bd6, 60s window:
    # one 1000ms CPU gap (cpu_ns == duration_ns), one 2000ms Lock:relation.
    # In a /tmp copy of compute.c, continue after db_time_ns += dur_ns for
    # that ONE Lock event, before class attribution. No loader/event loss:
    # DB Time stays 3000ms; the residual emits the lost 2000ms as Off-CPU.
    # These are the resulting rows, not an independently edited total.
    tm_rows = [
        {"name": "DB Time", "ms": 3000.0, "indent": 0},
        {"name": "CPU (running)", "ms": 1000.0, "indent": 1},
        {"name": "CPU (waiting for a core)", "ms": 2000.0, "indent": 1},
    ]
    te_rows = [
        {"class": "Lock", "name": "Lock:relation", "total_ms": 2000.0,
         "pct": 66.66666666666666},
        {"class": "CPU", "name": "CPU*", "total_ms": 1000.0,
         "pct": 33.33333333333333},
    ]
    tm_db_time_ms = tm_rows[0]["ms"]
    te_db_time_ms = 3000.0
    cons_ok, cons_detail = lib.time_model_conservation(tm_rows, tm_db_time_ms)
    check(cons_ok and "gap=0.0ms" in cons_detail,
          f"one dropped wait event is absorbed by Off-CPU ({cons_detail})")
    total_ok, total_detail = lib.cross_tab_db_time_agreement_ok(
        tm_db_time_ms, te_db_time_ms)
    check(total_ok and "gap=0.00%" in total_detail,
          f"cross_tab_db_time_agreement: PASS -- {total_detail}")
    class_ok, class_detail = lib.class_rows_vs_top_events_ok(
        tm_rows, te_rows, te_db_time_ms)
    check(not class_ok and
          "Lock: time_model=0.0ms vs top_events=2000.0ms (100.00%)" in class_detail,
          f"cross_tab_class_attribution: FAIL -- {class_detail}")
    check("CPU: time_model=3000.0ms vs top_events=1000.0ms" in class_detail,
          "the surviving gate also detects the 2000ms moved into Off-CPU")

    # Exercise the real mandatory set with every other check passing.
    checks = {name: {"ok": True} for name in lib.REQUIRED_EXTRA_CHECKS}
    checks["cross_tab_class_attribution"] = {"ok": class_ok}
    summary = lib.build_demo_summary(
        _passing_walk(), checks, required_checks=lib.REQUIRED_EXTRA_CHECKS)
    check(summary["failed"] == ["cross_tab_class_attribution"],
          "the dropped event fails the rehearsal verdict via the surviving gate")
    del checks["cross_tab_class_attribution"]
    summary = lib.build_demo_summary(
        _passing_walk(), checks, required_checks=lib.REQUIRED_EXTRA_CHECKS)
    check("missing_required_check:cross_tab_class_attribution" in summary["failed"],
          "omitting the surviving gate also fails the rehearsal verdict")


def test_class_attribution_class_missing_from_top_events_fails():
    # The mirror direction, and the bypass-suite case CLAUDE.md names
    # explicitly: the thing being checked is ABSENT rather than wrong. The
    # comparison walks the UNION of both sides' classes, so a class only
    # time_model reports is a failure, not a key the loop never visits.
    rows = _real_tm_rows() + [{"name": "IPC", "ms": 500.0, "indent": 1}]
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(not ok, f"a class present only in time_model fails ({detail})")
    check("IPC: time_model=500.0ms vs top_events=0.0ms" in detail,
          f"and is named with its own numbers (got: {detail})")


def test_class_attribution_within_tolerance_ok():
    rows = _real_tm_rows()
    for r in rows:
        if r["name"] == "Lock" and r["indent"] == 1:
            r["ms"] = 1990.0          # 0.5% under, inside the 1% bound
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(ok, f"a 0.5% per-class gap is inside the 1% tolerance ({detail})")


def test_class_attribution_just_outside_tolerance_fails():
    rows = _real_tm_rows()
    for r in rows:
        if r["name"] == "Lock" and r["indent"] == 1:
            r["ms"] = 1960.0          # 2% under
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(not ok, f"a 2% per-class gap fails the 1% tolerance ({detail})")


def test_class_attribution_legacy_single_cpu_row_folds():
    # Sampled/v2/legacy windows emit one "CPU*" row instead of the measured
    # pair. It must fold onto the same label, or this check would be red on
    # every legacy window instead of comparing it.
    rows = [r for r in _real_tm_rows()
            if r["name"] not in ("CPU (running)", "CPU (waiting for a core)")]
    rows.append({"name": "CPU*", "ms": 1980.0, "indent": 1})
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(ok, f"a legacy single CPU* row folds onto the CPU label ({detail})")


# -- class_rows_vs_top_events_ok: BYPASS SUITE ---------------------------
#
# One case per way this gate's detection can be made unreachable. Every one
# must REFUSE (ok False), never approve: a gate that cannot see the thing it
# compares has no business returning True.

def test_class_attribution_empty_time_model_rows_refuses():
    ok, detail = lib.class_rows_vs_top_events_ok([], _real_te_rows(),
                                                 REAL_TE_DB_TIME_MS)
    check(not ok, f"empty time_model rows refuse ({detail})")
    # Pinned to the SPECIFIC message, not just "void": every later guard
    # (no indent-1 rows, the MIN_DB_TIME_MS floor) also refuses empty input
    # and also says "void", so a looser assertion here would still pass
    # with this guard deleted and would stop pinning anything.
    check("no time_model rows" in detail,
          f"and names the empty side, not a downstream symptom (got: {detail})")


def test_class_attribution_empty_top_events_rows_refuses():
    ok, detail = lib.class_rows_vs_top_events_ok(_real_tm_rows(), [],
                                                 REAL_TE_DB_TIME_MS)
    check(not ok, f"empty top_events rows refuse ({detail})")
    check("no top_events rows" in detail,
          f"and names the empty side (got: {detail})")


def test_class_attribution_none_either_side_refuses():
    # A missing `rows` key reads as None at the call site.
    ok, _ = lib.class_rows_vs_top_events_ok(None, _real_te_rows(),
                                            REAL_TE_DB_TIME_MS)
    check(not ok, "a missing time_model rows key refuses")
    ok, _ = lib.class_rows_vs_top_events_ok(_real_tm_rows(), None,
                                            REAL_TE_DB_TIME_MS)
    check(not ok, "a missing top_events rows key refuses")


def test_class_attribution_no_indent1_rows_refuses():
    # Rows present, but the CLASS rows absent -- the comparison would
    # otherwise walk only top_events' classes against zeros, or (worse, if
    # it walked the intersection) nothing at all.
    rows = [r for r in _real_tm_rows() if r["indent"] != 1]
    ok, detail = lib.class_rows_vs_top_events_ok(rows, _real_te_rows(),
                                                 REAL_TE_DB_TIME_MS)
    check(not ok, f"a response with no indent-1 class rows refuses ({detail})")
    check("no indent-1" in detail, f"and names why (got: {detail})")


def test_class_attribution_all_top_events_rows_idle_refuses():
    rows = [dict(r, pct=None) for r in _real_te_rows()]
    ok, detail = lib.class_rows_vs_top_events_ok(_real_tm_rows(), rows,
                                                 REAL_TE_DB_TIME_MS)
    check(not ok, f"top_events with only idle-but-visible rows refuses "
                  f"({detail})")


def test_class_attribution_missing_pct_key_refuses():
    # Without `pct` there is no way to tell an idle-but-visible row from a
    # DB-Time one, so every idle row's time would be added to a class and
    # the comparison would be red (or, if the default went the other way,
    # silently drop real classes).
    rows = [{k: v for k, v in r.items() if k != "pct"} for r in _real_te_rows()]
    ok, detail = lib.class_rows_vs_top_events_ok(_real_tm_rows(), rows,
                                                 REAL_TE_DB_TIME_MS)
    check(not ok, f"a top_events row with no `pct` key refuses ({detail})")
    check("pct" in detail, f"and names the missing field (got: {detail})")


def test_class_attribution_unparseable_time_model_ms_refuses():
    for bad in (None, "2000.0", float("nan"), float("inf"), True):
        rows = _real_tm_rows()
        for r in rows:
            if r["name"] == "Lock" and r["indent"] == 1:
                r["ms"] = bad
        ok, detail = lib.class_rows_vs_top_events_ok(
            rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
        check(not ok, f"an unparseable time_model ms ({bad!r}) refuses "
                      f"({detail})")


def test_class_attribution_unparseable_top_events_total_ms_refuses():
    for bad in (None, "2000.0", float("nan"), True):
        rows = [dict(r) for r in _real_te_rows()]
        rows[0]["total_ms"] = bad
        ok, detail = lib.class_rows_vs_top_events_ok(
            _real_tm_rows(), rows, REAL_TE_DB_TIME_MS)
        check(not ok, f"an unparseable top_events total_ms ({bad!r}) refuses "
                      f"({detail})")


def test_class_attribution_unknown_class_label_refuses():
    # A class label neither side's table knows -- e.g. a twelfth wait class
    # added to src/compute.h, or a renamed display name. Dropping it would
    # remove that class's time from the comparison without any signal.
    rows = _real_tm_rows()
    for r in rows:
        if r["name"] == "Lock" and r["indent"] == 1:
            r["name"] = "Quantum"
    ok, detail = lib.class_rows_vs_top_events_ok(
        rows, _real_te_rows(), REAL_TE_DB_TIME_MS)
    check(not ok, f"an unknown time_model class-row name refuses ({detail})")
    check("Quantum" in detail, f"and names it (got: {detail})")

    te = [dict(r) for r in _real_te_rows()]
    te[0]["class"] = "Quantum"
    ok, detail = lib.class_rows_vs_top_events_ok(
        _real_tm_rows(), te, REAL_TE_DB_TIME_MS)
    check(not ok, f"an unknown top_events class label refuses ({detail})")


def test_class_attribution_unknown_label_on_BOTH_sides_refuses():
    # The case a "skip what you do not recognise" implementation gets
    # wrong: when BOTH endpoints rename the same class, skipping it drops
    # that class's time out of the comparison on both sides at once, so no
    # per-class gap ever appears and the gate approves without having
    # looked at it. Refusing is the only safe answer.
    tm = _real_tm_rows()
    for r in tm:
        if r["name"] == "Lock" and r["indent"] == 1:
            r["name"] = "Quantum"
    te = [dict(r) for r in _real_te_rows()]
    for r in te:
        if r["class"] == "Lock":
            r["class"] = "Quantum"
    ok, detail = lib.class_rows_vs_top_events_ok(tm, te, REAL_TE_DB_TIME_MS)
    check(not ok, f"an unknown label on both sides refuses rather than "
                  f"quietly dropping the class ({detail})")
    check("Quantum" in detail, f"and names it (got: {detail})")


def test_class_attribution_missing_class_field_refuses():
    for bad in (None, "", 7):
        te = [dict(r) for r in _real_te_rows()]
        te[0]["class"] = bad
        ok, detail = lib.class_rows_vs_top_events_ok(
            _real_tm_rows(), te, REAL_TE_DB_TIME_MS)
        check(not ok, f"a top_events row with class={bad!r} refuses "
                      f"({detail})")


def test_class_attribution_truncated_top_events_row_list_refuses():
    # A truncated row list shrinks one or more classes on the top_events
    # side. It is detectable because top_events' own db_time_ms is
    # accumulated over the same records, so the DB-Time rows must add up to
    # it. Without this precondition the check would read the truncation as
    # a per-class disagreement (noisy) or, if the missing class were absent
    # from BOTH sides, as agreement.
    te = [r for r in _real_te_rows() if r["class"] != "IO"]
    ok, detail = lib.class_rows_vs_top_events_ok(
        _real_tm_rows(), te, REAL_TE_DB_TIME_MS)
    check(not ok, f"a truncated top_events row list refuses ({detail})")
    check("incomplete" in detail,
          f"and says the row list is incomplete rather than blaming a class "
          f"({detail})")


def test_class_attribution_unparseable_top_events_db_time_refuses():
    for bad in (None, "7980", float("nan"), True):
        ok, detail = lib.class_rows_vs_top_events_ok(
            _real_tm_rows(), _real_te_rows(), bad)
        check(not ok, f"top_events db_time_ms={bad!r} refuses ({detail})")


def test_class_attribution_below_nonzero_floor_refuses():
    # Both sides agreeing at ~zero is the 0 ~= 0 reading MIN_DB_TIME_MS
    # exists to reject: in this harness (continuous load for the whole
    # capture) it means nothing was captured, not that nothing happened.
    tm = [{"name": "DB Time", "ms": 0.0002, "indent": 0},
          {"name": "Lock", "ms": 0.0001, "indent": 1},
          {"name": "CPU (running)", "ms": 0.0001, "indent": 1}]
    te = [{"class": "Lock", "name": "Lock:extend", "total_ms": 0.0001, "pct": 50.0},
          {"class": "CPU", "name": "CPU*", "total_ms": 0.0001, "pct": 50.0}]
    ok, detail = lib.class_rows_vs_top_events_ok(tm, te, 0.0002)
    check(not ok, f"a near-zero window refuses rather than agreeing at zero "
                  f"({detail})")
    check("floor" in detail, f"and names the floor (got: {detail})")


def test_class_attribution_non_dict_top_events_row_refuses():
    te = _real_te_rows() + ["not-a-row"]
    ok, detail = lib.class_rows_vs_top_events_ok(
        _real_tm_rows(), te, REAL_TE_DB_TIME_MS)
    check(not ok, f"a non-object top_events row refuses ({detail})")


def test_class_attribution_is_a_mandatory_rehearsal_check():
    # Without this, the leg could be computed, printed, and never gate --
    # which is #464's failure mode (absent, not wrong).
    check("cross_tab_class_attribution" in lib.REQUIRED_EXTRA_CHECKS,
          "cross_tab_class_attribution is in REQUIRED_EXTRA_CHECKS "
          "(test_required_check_names_all_exist_in_demo_rehearsal then "
          "forces demo_rehearsal.py to actually insert it)")
    check("cross_tab_db_time_agreement" not in lib.REQUIRED_EXTRA_CHECKS,
          "the redundant total diagnostic is no longer mandatory (#258)")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "demo_rehearsal.py")
    with open(path) as fh:
        source = fh.read()
    check("drlib.cross_tab_db_time_agreement_ok(" not in source and
          'extra_checks["cross_tab_db_time_agreement"]' not in source,
          "the rehearsal neither runs nor gates the historical total diagnostic")


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


def test_bucket_weighted_aas_ok_catches_an_endpoint_level_divergence():
    # #517 asked whether this leg can go red at all, given that its live
    # configuration (60 buckets over 60 s) makes a weighted and an
    # unweighted derivation coincide. It can -- but via the comparison it
    # actually gates: the `aas` endpoint's own accumulation against
    # time_model's `aas` for the same window. 60 x 1s buckets totalling
    # 2.0 AAS each derive 2.0; a time_model reporting 2.1 is a 4.8%
    # cross-endpoint disagreement and fails.
    buckets = [{"t": i * 1_000_000_000, "cpu": 1.2, "io": 0.8}
               for i in range(60)]
    ok, detail = lib.bucket_weighted_aas_ok(
        buckets, 1_000_000_000, 0, 60_000_000_000, 2.0)
    check(ok, f"the two endpoints agreeing passes ({detail})")
    ok, detail = lib.bucket_weighted_aas_ok(
        buckets, 1_000_000_000, 0, 60_000_000_000, 2.1)
    check(not ok, f"a 4.8% cross-endpoint disagreement FAILS in the very "
                  f"60-buckets-over-60s geometry #517 called degenerate "
                  f"({detail})")
    check("derived=2.0000 time_model=2.1000" in detail,
          f"and states both endpoints' numbers (got: {detail})")


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


# ── cross_tab_bucket_ns overshoot bound (issue #570) ──────────────────────
#
# PR #564 concluded the cross-tab AAS leg is sound because
# src/compute.c:797's ceiling bucket_ns formula bounds the overshoot
# `N*bucket_ns - range_ns` to [0, N-1] nanoseconds. This pins that bound
# (not the specific 9.11e-7% figure the probe measured) plus its
# consequence for the weighted-vs-unweighted comparison, and a bypass-suite
# case proving the bound test is not vacuous.
#
# Representative spread: the five AAS_CROSS_TAB_NUM_BUCKETS-scale bucket
# counts and the five window spans PR #564's probe measured against
# (12.3-90.1s, 7/59/60/61/600 buckets) -- all 25 combinations, not just
# the 16 the probe happened to hit, since the bound must hold for every
# (N, range) pair.
_BOUND_TEST_N = (7, 59, 60, 61, 600)
_BOUND_TEST_SPANS_S = (12.3, 45.0, 60.0, 61.000000001, 90.1)


def test_cross_tab_bucket_ns_overshoot_bound_holds():
    for n in _BOUND_TEST_N:
        for span_s in _BOUND_TEST_SPANS_S:
            range_ns = int(span_s * 1e9)
            bucket_ns = lib.cross_tab_bucket_ns(range_ns, n)
            overshoot = n * bucket_ns - range_ns
            check(0 <= overshoot <= n - 1,
                  f"N={n} range_ns={range_ns}: overshoot={overshoot} must "
                  f"be in [0, {n - 1}] ns (bucket_ns={bucket_ns})")


def test_cross_tab_bucket_ns_overshoot_bound_breaks_under_truncation():
    # BYPASS-SUITE CASE -- if src/compute.c:797's ceiling division were
    # ever replaced with plain truncation (floor), N full-width buckets
    # would UNDERSHOOT the window instead (a negative "overshoot"),
    # failing the lower bound -- exactly the mutation issue #570 requires
    # this test to catch, and PR #559's review found a sibling truncation
    # at compute.c:1845 left a related test green. This demonstrates the
    # bound test above is not vacuous: it is sensitive to ceil vs.
    # truncate specifically, not satisfied by just "some division
    # formula". Most (N, span) pairs below are NOT evenly divisible
    # (wall-clock windows essentially never are, per
    # bucket_weighted_aas_ok's own module comment), so truncation fails
    # the bound for the large majority of them.
    total = len(_BOUND_TEST_N) * len(_BOUND_TEST_SPANS_S)
    failures = 0
    for n in _BOUND_TEST_N:
        for span_s in _BOUND_TEST_SPANS_S:
            range_ns = int(span_s * 1e9)
            truncated_bucket_ns = range_ns // n  # the mutation: ceil -> floor
            overshoot = n * truncated_bucket_ns - range_ns
            if not (0 <= overshoot <= n - 1):
                failures += 1
    check(failures >= total // 2,
          f"truncation breaks the overshoot bound for a majority of "
          f"geometries ({failures}/{total}), proving the bound test above "
          f"actually depends on ceil rather than passing for any formula")


def test_cross_tab_weighted_vs_unweighted_error_has_wide_margin():
    # Pins the CONSEQUENCE of the overshoot bound: for a uniform-rate
    # workload (every bucket accumulates busy_ns at the same rate `r`; the
    # server still divides the truncated tail bucket's partial
    # accumulation by the FULL bucket_ns -- see bucket_weighted_aas_ok's
    # module comment), the gap between an UNWEIGHTED mean of bucket aas
    # values and the true rate stays many orders of magnitude below
    # TIME_MODEL_TOLERANCE_PCT. This is the "left alone" argument from
    # issue #570 / PR #564: the error this leg could ever introduce is
    # nanosecond-scale against multi-millisecond-or-larger buckets, so it
    # can never approach the 1% gate.
    #
    # "weighted" and "unweighted" are produced INDEPENDENTLY here, not
    # through a shared helper: weighted_derived comes from
    # bucket_weighted_aas_ok's own internal formula
    # (sum(aas_i*bucket_ns)/range_ns, exercised via its (ok, detail)
    # return); unweighted_mean is computed inline below with a DIFFERENT
    # formula (sum(aas_i)/num_buckets) -- a mutation that breaks one does
    # not automatically move the other.
    rate = 2.0
    worst_gap_pct = 0.0
    for n in _BOUND_TEST_N:
        for span_s in _BOUND_TEST_SPANS_S:
            range_ns = int(span_s * 1e9)
            bucket_ns = lib.cross_tab_bucket_ns(range_ns, n)
            last_width_ns = range_ns - (n - 1) * bucket_ns
            aas_values = [rate] * (n - 1) + [rate * last_width_ns / bucket_ns]
            buckets = [{"t": i * bucket_ns, "cpu": a}
                       for i, a in enumerate(aas_values)]

            ok, detail = lib.bucket_weighted_aas_ok(
                buckets, bucket_ns, 0, range_ns, rate)
            check(ok, f"weighted derivation matches the true rate "
                      f"(N={n} span={span_s}s): {detail}")

            unweighted_mean = sum(aas_values) / n
            gap_pct = abs(unweighted_mean - rate) / rate * 100.0
            worst_gap_pct = max(worst_gap_pct, gap_pct)

    margin = lib.TIME_MODEL_TOLERANCE_PCT / 10_000
    check(worst_gap_pct < margin,
          f"worst unweighted-vs-true-rate gap across {len(_BOUND_TEST_N)}x"
          f"{len(_BOUND_TEST_SPANS_S)} geometries is {worst_gap_pct:.2e}%, "
          f"must stay under {margin:.2e}% (tolerance "
          f"{lib.TIME_MODEL_TOLERANCE_PCT}% / 10000x margin)")


# ── Coupling check: does src/compute.c still USE the ceiling formula? ─────
#
# The three tests above prove the overshoot bound holds FOR THE CEILING
# FORMULA -- they say nothing about whether src/compute.c still computes
# bucket_ns that way. cross_tab_bucket_ns() in demo_rehearsal_lib.py is a
# reimplementation (its own docstring says so); a reimplementation can
# drift from its original silently, exactly the #589 class of bug (a C
# literal renamed, a JS copy of it left stale through a full `make check`,
# caught only by a human grep during integration).
#
# So: extract the REAL formula text out of src/compute.c's pgwt_compute_aas
# (never a second hardcoded copy of the formula itself) and assert,
# structurally, that it is still a ceiling-division shape -- the divisor
# appearing both as the "+B-1" addend and as the final divisor, whatever
# that variable happens to be called. Raises (refuses) rather than
# reporting "nothing found, nothing to check" when the function or the
# assignment can't be located, per issue #570's bypass-suite requirement.
COMPUTE_C_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "src", "compute.c")


def extract_cross_tab_bucket_ns_statement(compute_c_source):
    """Extract the literal `uint64_t bucket_ns = ...;` statement from
    inside pgwt_compute_aas (the AAS endpoint -- the same function
    bucket_weighted_aas_ok's bucket_ns argument comes from). Anchored on
    the function name so the THREE other near-identical bucket_ns
    assignments elsewhere in compute.c (for other endpoints, not gated by
    this issue) are never matched instead. Raises if the function or the
    assignment can't be found -- a reworded anchor must fail loudly, not
    silently pass having checked nothing."""
    func_match = re.search(r'\bpgwt_compute_aas\s*\(', compute_c_source)
    if not func_match:
        raise AssertionError(
            "#570: could not find pgwt_compute_aas(...) in src/compute.c "
            "-- the function was renamed or moved; update the extractor "
            "rather than letting the coupling check pass on no evidence")
    window = compute_c_source[func_match.start():func_match.start() + 4000]
    stmt_match = re.search(r'uint64_t\s+bucket_ns\s*=\s*(.+?);', window)
    if not stmt_match:
        raise AssertionError(
            "#570: could not find a `uint64_t bucket_ns = ...;` "
            "assignment inside pgwt_compute_aas in src/compute.c -- the "
            "statement was reworded; update the extractor rather than "
            "letting the coupling check pass on no evidence")
    return stmt_match.group(0).strip()


def bucket_ns_statement_is_ceiling_division(statement):
    """Structural check, not a literal-string copy of the formula: matches
    `uint64_t bucket_ns = (A + [cast]B - 1) / [cast]C;` and returns
    whether B and C are the SAME variable (any cast stripped) -- the
    shape that makes it ceiling division of A by that divisor. Returns
    None (not False) when the statement doesn't even have this shape at
    all (e.g. plain truncating division with no "+ ... - 1" term) so
    callers can tell "found a non-ceiling assignment" apart from "found
    nothing" if they need to, though issue #570's own gate treats both as
    not-proven-ceiling."""
    m = re.match(
        r'uint64_t\s+bucket_ns\s*=\s*\(\s*(?P<a>[\w.]+)\s*\+\s*'
        r'(?:\(\s*[A-Za-z_][A-Za-z_0-9 ]*\s*\)\s*)?(?P<b>\w+)\s*-\s*1\s*\)'
        r'\s*/\s*(?:\(\s*[A-Za-z_][A-Za-z_0-9 ]*\s*\)\s*)?(?P<c>\w+)\s*;',
        statement)
    if not m:
        return None
    return m.group('b') == m.group('c')


def test_compute_c_cross_tab_bucket_ns_is_still_ceiling_division():
    with open(COMPUTE_C_PATH, "r") as f:
        source = f.read()
    statement = extract_cross_tab_bucket_ns_statement(source)
    is_ceiling = bucket_ns_statement_is_ceiling_division(statement)
    check(is_ceiling is True,
          f"src/compute.c's AAS bucket_ns assignment is still ceiling "
          f"division -- the premise the three overshoot-bound tests above "
          f"depend on ({statement!r})")


def test_extract_cross_tab_bucket_ns_statement_refuses_missing_function():
    try:
        extract_cross_tab_bucket_ns_statement(
            "void some_other_function(void) { int x = 1; }")
        check(False, "should have raised when pgwt_compute_aas is absent")
    except AssertionError as e:
        check("#570" in str(e), f"raises a #570-tagged error ({e})")


def test_extract_cross_tab_bucket_ns_statement_refuses_empty_source():
    try:
        extract_cross_tab_bucket_ns_statement("")
        check(False, "should have raised on an empty file")
    except AssertionError as e:
        check("#570" in str(e), f"raises a #570-tagged error ({e})")


def test_extract_cross_tab_bucket_ns_statement_refuses_reworded_assignment():
    # Function is found, but the assignment itself was reworded to
    # something the statement regex can't parse at all (e.g. split across
    # a helper call) -- must still refuse, not silently find nothing and
    # report success.
    source = ("void pgwt_compute_aas(uint64_t from_ns, uint64_t to_ns, "
               "int num_buckets) {\n"
               "    uint64_t bucket_ns = compute_bucket_width(from_ns, "
               "to_ns, num_buckets)\n"  # no trailing ';' on this line
               "        + extra_padding_ns;\n"
               "}\n")
    try:
        extract_cross_tab_bucket_ns_statement(source)
        check(False, "should have raised when the assignment has no "
                      "single-statement `= ...;` shape on one line")
    except AssertionError as e:
        check("#570" in str(e), f"raises a #570-tagged error ({e})")


def test_bucket_ns_statement_is_ceiling_division_true_for_the_real_shape():
    check(bucket_ns_statement_is_ceiling_division(
        "uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / "
        "(uint64_t)num_buckets;") is True,
        "the real cast-and-all shape is recognised as ceiling division")


def test_bucket_ns_statement_is_ceiling_division_true_without_casts():
    check(bucket_ns_statement_is_ceiling_division(
        "uint64_t bucket_ns = (range_ns + num_buckets - 1) / num_buckets;")
        is True, "a cast-free ceiling shape is recognised too")


def test_bucket_ns_statement_is_ceiling_division_false_for_truncation():
    # BYPASS-SUITE CASE -- the exact mutation issue #570 requires this
    # coupling check to catch: ceil replaced by plain truncating division.
    result = bucket_ns_statement_is_ceiling_division(
        "uint64_t bucket_ns = range_ns / (uint64_t)num_buckets;")
    check(result is not True,
          f"plain truncating division is NOT recognised as ceiling "
          f"division (got {result!r})")


def test_bucket_ns_statement_is_ceiling_division_false_for_mismatched_divisor():
    # The addend and the final divisor must be the SAME variable -- a
    # formula that adds one variable's "-1" but divides by a different
    # one is not ceiling division of range_ns by that divisor at all.
    result = bucket_ns_statement_is_ceiling_division(
        "uint64_t bucket_ns = (range_ns + (uint64_t)num_buckets - 1) / "
        "(uint64_t)other_var;")
    check(result is False,
          f"a mismatched addend/divisor is explicitly False, not treated "
          f"as ceiling (got {result!r})")


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
    summary = lib.summarize_sweep_offset_coverage("scatter", "early", ticks, 500)
    check(summary["tab"] == "scatter" and summary["pass"] == "early",
          "tab/pass identity carried through")
    check(len(summary["ticks"]) == 2, "one drift record per input tick")
    check(summary["ticks"][1]["first_drift_ms"] == 469,
          f"the late-mount tick's drift is visible in the summary ({summary['ticks'][1]})")


def test_summarize_sweep_offset_coverage_empty_ticks():
    summary = lib.summarize_sweep_offset_coverage("overview", "early", [], 500)
    check(summary["ticks"] == [], "no ticks -> empty list, not a crash")
    summary_none = lib.summarize_sweep_offset_coverage("overview", "early", None, 500)
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


# ── cmd_markers_observed (#222 review) ────────────────────────────────────

def test_cmd_markers_observed_nonzero_reports_seen():
    ok, count, detail = lib.cmd_markers_observed({"live_cmd_markers_total": 5})
    check(ok is True and count == 5,
          f"a nonzero count reports seen=True with the real count ({detail})")
    check("fired live" in detail, f"detail says markers fired ({detail})")


def test_cmd_markers_observed_zero_reports_not_seen_not_a_failure():
    # BYPASS-SUITE CASE (the review's own point): zero is a real, expected
    # outcome for a workload that never escalates a backend to exact tier
    # -- this function itself must not turn that into an exception or a
    # crash, and its caller (demo_rehearsal.py) must not gate on it.
    ok, count, detail = lib.cmd_markers_observed({"live_cmd_markers_total": 0})
    check(ok is False and count == 0,
          f"a zero count reports seen=False, not an exception ({detail})")
    check("UNPROVEN" in detail, f"detail names the coverage gap explicitly ({detail})")


def test_cmd_markers_observed_missing_field_fails_closed():
    # BYPASS-SUITE CASE: an older daemon/protocol without this field, or a
    # truncated metrics blob -- must not silently read as "zero markers,
    # fine", the same fail-closed contract as daemon_integrity_ok.
    ok, count, detail = lib.cmd_markers_observed({"some_other_field": 1})
    check(ok is False and count == 0,
          f"a missing field reports seen=False, not treated as zero-and-fine ({detail})")
    check("missing" in detail, f"detail names the field as missing ({detail})")


def test_cmd_markers_observed_non_dict_response_fails():
    ok, count, detail = lib.cmd_markers_observed(None)
    check(ok is False and count == 0,
          f"a non-dict metrics response fails outright, not seen=True ({detail})")


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


def test_daemon_log_clean_degraded_warn_refuses():
    log = "WARN: CPU accounting: LEGACY gap-inference -- degraded mode\n"
    ok, errors, warnings = lib.daemon_log_clean(log)
    check(not ok, "an announced degraded tier refuses a full-mode rehearsal")
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
    rendered_ok=True, rendered_detail="ok", ticks_observed=6, ttfp_ms=842,
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


# ── resolve_tag: --tag plumbing (docs/DEMO_REHEARSAL_CRITERIA.md "a
# counted attempt names the tag it ran against, ... same tag, not merely
# the same tree") ────────────────────────────────────────────────────────

def test_resolve_tag_uses_cli_value_when_given():
    auto_detect_calls = []
    def auto_detect():
        auto_detect_calls.append(1)
        return "should-not-be-used"
    tag = lib.resolve_tag("v1.0.0", auto_detect)
    check(tag == "v1.0.0", f"a passed --tag reaches the result unchanged ({tag!r})")
    check(not auto_detect_calls,
          "a non-empty CLI tag short-circuits auto-detection entirely "
          "(never even calls it)")


def test_resolve_tag_falls_back_to_autodetect_when_absent():
    # BYPASS-SUITE CASE: "absent" must mean None is a live possibility, not
    # a gate that can never fire (auto_detect_fn genuinely gets called and
    # its real answer -- including a found tag -- is used).
    tag = lib.resolve_tag(None, lambda: "auto-detected-tag")
    check(tag == "auto-detected-tag",
          f"no CLI tag -> the auto-detected tag is used ({tag!r})")


def test_resolve_tag_falls_back_to_autodetect_when_empty_string():
    # A shell layer threading an unset env var through (TAG="${5:-}") can
    # produce "" rather than Python's argparse default of None -- both must
    # be treated as "not given", never recorded as a literal "" tag.
    tag = lib.resolve_tag("", lambda: "auto-detected-tag")
    check(tag == "auto-detected-tag",
          f"an empty-string CLI tag also falls through to auto-detect ({tag!r})")


def test_resolve_tag_null_when_nothing_available():
    # The honest-absence case this whole mechanism exists to preserve:
    # docs/DEMO_REHEARSAL_CRITERIA.md's _detect_git_tag() docstring is
    # explicit that failing to find an exact tag is "the correct, honest
    # answer", not an error -- an attempt run from a non-tagged tree (or
    # where .git is unavailable, e.g. the remote copy scripts/
    # demo-rehearsal.sh rsyncs with --exclude .git) must still record
    # tag: null, never a commit sha or any other invented substitute.
    tag = lib.resolve_tag(None, lambda: None)
    check(tag is None,
          f"no CLI tag and no auto-detected tag -> null, not invented ({tag!r})")
    tag = lib.resolve_tag("", lambda: None)
    check(tag is None,
          f"empty CLI tag and no auto-detected tag -> still null ({tag!r})")


# ── evaluate_execution_cardinality (#464) ────────────────────────────────
#
# The recorded defect, verbatim from the archived rehearsal
# (tests/results/demo-video-48e1bce): the DML query carried exec_count 368
# against plan_count 184, the SELECT 91 against 91. Both counted rehearsals
# PASSED in that state, because nothing looked at counts.

RECORDED_DML_ROW = {"query_id": "6618837284", "exec_count": 368,
                    "plan_count": 184,
                    "text": "UPDATE pgbench_accounts SET abalance = "
                            "abalance + $1 WHERE aid = $2"}
RECORDED_SELECT_ROW = {"query_id": "1193740", "exec_count": 91,
                       "plan_count": 91,
                       "text": "SELECT abalance FROM pgbench_accounts "
                               "WHERE aid = $1"}


def test_exec_cardinality_recorded_368_184_fails():
    # Negative control 2 from the brief: feed the evaluator the number the
    # rehearsal actually recorded. If this passes, the gate is decoration.
    r = lib.evaluate_execution_cardinality([RECORDED_DML_ROW,
                                            RECORDED_SELECT_ROW])
    check(not r["ok"],
          f"recorded 368/184 must FAIL (got ok={r['ok']}, {r['detail']})")
    check(any("368" in f for f in r["failures"]),
          f"failure names the doubled count ({r['failures']})")
    check(r["evaluated"] == 2,
          f"both rows evaluated, so the SELECT is not what failed "
          f"({r['evaluated']})")


def test_exec_cardinality_select_alone_passes():
    # The control that CANNOT see the defect: a SELECT never reaches the
    # second probe site, so 91/91 was correct before the fix and after it.
    # If this failed, the check would be measuring something else.
    r = lib.evaluate_execution_cardinality([RECORDED_SELECT_ROW])
    check(r["ok"], f"91/91 must pass ({r['detail']})")


def test_exec_cardinality_equal_counts_pass_and_edge_slack_is_small():
    rows = [{"text": "UPDATE t SET v = 1", "exec_count": 24,
             "plan_count": 24}]
    check(lib.evaluate_execution_cardinality(rows)["ok"],
          "24/24 passes")
    # One open plan at each window edge is tolerated; a factor is not.
    for execs, want_ok in ((26, True), (27, False), (22, True), (21, False)):
        rows = [{"text": "UPDATE t SET v = 1", "exec_count": execs,
                 "plan_count": 24}]
        got = lib.evaluate_execution_cardinality(rows)["ok"]
        check(got is want_ok,
              f"exec {execs} vs plan 24 -> ok={got}, expected {want_ok}")
    check(lib.EXEC_CARDINALITY_EDGE_SLACK == 2,
          f"edge slack stays at window edges, not a tolerance "
          f"({lib.EXEC_CARDINALITY_EDGE_SLACK})")


def test_exec_cardinality_independent_n_catches_uniform_doubling():
    # Negative control 3: Plans preserved, Execs doubled -- the shape an
    # extended/prepared cohort has, where exec == plan cannot apply and
    # plan <= exec passes at ANY multiple. Only the independently counted N
    # can see it.
    doubled = [{"text": "EXECUTE-equivalent UPDATE t SET v = 1 WHERE id = 9",
                "exec_count": 48, "plan_count": 6}]
    loose = lib.evaluate_execution_cardinality(doubled)
    check(not loose["ok"],
          "48 execs against 6 plans fails the ratio leg too")
    # And the leg that matters for prepared statements, where plans are
    # legitimately few: exec must equal the client's own count.
    prepared = [{"text": "UPDATE t SET v = 1 WHERE id = 9",
                 "exec_count": 48, "plan_count": 48}]
    r = lib.evaluate_execution_cardinality(prepared, expected={"id = 9": 24})
    check(not r["ok"],
          f"exec 48 against an independently counted N=24 must FAIL "
          f"({r['detail']})")
    ok_rows = [{"text": "UPDATE t SET v = 1 WHERE id = 9",
                "exec_count": 24, "plan_count": 24}]
    check(lib.evaluate_execution_cardinality(
              ok_rows, expected={"id = 9": 24})["ok"],
          "exec 24 == N=24 passes")


def test_exec_cardinality_missing_expected_row_fails():
    # "The statement I issued 24 times produced no row" is what a refusing
    # or mis-attached capture looks like. It must never read as agreement.
    r = lib.evaluate_execution_cardinality(
        [{"text": "SELECT 1", "exec_count": 3, "plan_count": 3}],
        expected={"UPDATE pgwt464": 24})
    check(not r["ok"], f"absent expected statement must FAIL ({r['detail']})")
    check(any("no evaluable row" in f for f in r["failures"]),
          f"failure says the row is missing ({r['failures']})")


def test_exec_cardinality_refuses_when_it_cannot_see():
    # Every way the evaluator could be handed nothing to look at. Each must
    # REFUSE -- an empty verdict is the false negative, not a safe default.
    cases = {
        "empty list": [],
        "rows absent (None)": None,
        "not iterable": 7,
        "all utility statements": [
            {"text": "SET application_name = 'x'", "exec_count": 9,
             "plan_count": 0},
            {"text": "EXECUTE p464(9)", "exec_count": 24, "plan_count": 0},
            {"text": "BEGIN", "exec_count": 4, "plan_count": 0}],
        "no planning captured": [
            {"text": "UPDATE t SET v = 1", "exec_count": 24,
             "plan_count": 0}],
        "counts unparseable": [
            {"text": "UPDATE t SET v = 1", "exec_count": "24",
             "plan_count": None}],
        "counts negative": [
            {"text": "UPDATE t SET v = 1", "exec_count": -1,
             "plan_count": 24}],
        "rows are not dicts": ["UPDATE t SET v = 1", None],
    }
    for label, rows in cases.items():
        r = lib.evaluate_execution_cardinality(rows)
        check(not r["ok"], f"{label} -> REFUSE, not pass (ok={r['ok']})")
        check(r["evaluated"] == 0,
              f"{label} -> evaluated 0 (got {r['evaluated']})")


def test_exec_cardinality_unparseable_count_is_not_zero():
    # The specific way "unparseable" becomes a pass: treating a missing or
    # string count as 0 makes exec == plan == 0 and the row agrees with
    # itself. The real row next to it must still be judged.
    rows = [{"text": "UPDATE t SET v = 1", "exec_count": None,
             "plan_count": None},
            {"text": "UPDATE u SET v = 1", "exec_count": 48,
             "plan_count": 24}]
    r = lib.evaluate_execution_cardinality(rows)
    check(not r["ok"], "an unreadable row does not shelter a doubled one")
    check(r["evaluated"] == 1,
          f"only the readable row is evaluated ({r['evaluated']})")
    check(any("not usable counts" in s for s in r["skipped"]),
          f"the unreadable row is reported as skipped ({r['skipped']})")


def test_exec_cardinality_abandoned_plans_are_skipped_not_failed():
    # Measured on gate-1: a deliberately failing `UPDATE ... SET v = v / 0`
    # reports exec_count 0 against plan_count 4 -- planned, then errored
    # before EXEC_DONE. The brief scopes the equality to complete,
    # successful statements, so this is a SKIP, and it must not drag down a
    # run whose real statements are fine.
    rows = [{"text": "UPDATE pgwt464_f SET v = v / 0 WHERE id = 5",
             "exec_count": 0, "plan_count": 4},
            {"text": "UPDATE pgwt464_u SET v = v + 1 WHERE id = 7",
             "exec_count": 16, "plan_count": 16}]
    r = lib.evaluate_execution_cardinality(rows)
    check(r["ok"], f"an abandoned-plan row is skipped, not failed "
                   f"({r['detail']})")
    check(r["evaluated"] == 1,
          f"only the successful statement is evaluated ({r['evaluated']})")
    check(any("abandoned plans" in s for s in r["skipped"]),
          f"the skip says why ({r['skipped']})")
    # But if the execution probes stopped firing ALTOGETHER, every plannable
    # row lands in that bucket -- and then there is nothing to evaluate, so
    # the verdict must be a refusal, not the pass the skip would otherwise
    # imply. This is the pairing that makes the skip above safe.
    allzero = [{"text": "UPDATE pgwt464_u SET v = v + 1", "exec_count": 0,
                "plan_count": 16},
               {"text": "SELECT count(*) FROM pgwt464_s", "exec_count": 0,
                "plan_count": 16}]
    r = lib.evaluate_execution_cardinality(allzero)
    check(not r["ok"],
          f"all executions missing -> REFUSE, never a pass ({r['detail']})")
    # And a cohort with an independently counted N that produced no
    # evaluable row is a FAILURE even though the row exists.
    r = lib.evaluate_execution_cardinality(
        rows, expected={"pgwt464_f": 4})
    check(not r["ok"],
          f"an expected cohort with zero completed executions FAILS "
          f"({r['detail']})")


def test_exec_cardinality_ambiguous_cohort_fails():
    # Two rows matching one expected key means the cohort was not isolated,
    # so "which one is the 24?" has no answer. Refuse rather than pick.
    rows = [{"text": "UPDATE pgwt464 SET v = v + 1 WHERE id = 7",
             "exec_count": 24, "plan_count": 24},
            {"text": "UPDATE pgwt464 SET v = v + 1 WHERE id = 7 -- copy",
             "exec_count": 24, "plan_count": 24}]
    r = lib.evaluate_execution_cardinality(rows, expected={"pgwt464": 24})
    check(not r["ok"], f"ambiguous cohort match must FAIL ({r['detail']})")


# ── build_demo_summary: required-check MEMBERSHIP (#464) ─────────────────

def _passing_walk():
    return [{"pass": "early", "tabs": {"overview": {"ok": True}}}]


def test_required_check_membership_rejects_a_deleted_check():
    # Negative control 4: remove the check from extra_checks. Before this,
    # build_demo_summary iterated extra_checks and so could only fail on an
    # entry that was PRESENT and failing -- a deleted one disappeared from
    # the verdict and the run printed PASS.
    required = ("alpha", "beta")
    full = {"alpha": {"ok": True}, "beta": {"ok": True}}
    s = lib.build_demo_summary(_passing_walk(), full, required_checks=required)
    check(s["ok"], f"all required checks present -> PASS ({s['failed']})")
    check(s["missing_required_checks"] == [],
          "nothing reported missing when nothing is missing")

    del full["beta"]
    s = lib.build_demo_summary(_passing_walk(), full, required_checks=required)
    check(not s["ok"], "a deleted required check must FAIL the verdict")
    check("missing_required_check:beta" in s["failed"],
          f"the missing check is named ({s['failed']})")
    check(s["missing_required_checks"] == ["beta"],
          f"summary.json records which one ({s['missing_required_checks']})")


def test_required_check_membership_is_not_satisfied_by_a_renamed_check():
    # The "some entry changed somewhere" false negative: the dict is
    # non-empty and every entry passes, so the count-based floor
    # (len(extra_checks) > 0) is satisfied while the check that matters is
    # gone under a different key.
    s = lib.build_demo_summary(
        _passing_walk(),
        {"execution_cardinalty": {"ok": True}},   # typo, deliberately
        required_checks=("execution_cardinality",))
    check(not s["ok"], "a typo'd key does not satisfy membership")
    check("missing_required_check:execution_cardinality" in s["failed"],
          f"the required name is named ({s['failed']})")


def test_required_check_membership_still_fails_a_present_failing_check():
    # Membership must ADD to the old behaviour, not replace it.
    s = lib.build_demo_summary(
        _passing_walk(), {"execution_cardinality": {"ok": False}},
        required_checks=("execution_cardinality",))
    check(not s["ok"], "a present-but-failing required check still fails")
    check("execution_cardinality" in s["failed"],
          f"and by its own name, not as 'missing' ({s['failed']})")
    check("missing_required_check:execution_cardinality" not in s["failed"],
          "a present check is never reported as missing")


def test_required_check_list_is_not_empty():
    # An empty REQUIRED_EXTRA_CHECKS makes the membership gate unreachable
    # while leaving every call site looking wired.
    check(len(lib.REQUIRED_EXTRA_CHECKS) > 0,
          "REQUIRED_EXTRA_CHECKS is non-empty")
    check("execution_cardinality" in lib.REQUIRED_EXTRA_CHECKS,
          "#464's cardinality check is mandatory")
    check(len(set(lib.REQUIRED_EXTRA_CHECKS))
          == len(lib.REQUIRED_EXTRA_CHECKS),
          "no duplicate names (a duplicate hides a drop)")


def test_required_check_names_all_exist_in_demo_rehearsal():
    # The gate's other unreachable path: a required name nobody ever
    # inserts would fail EVERY run, which gets "fixed" by deleting the
    # requirement. Every name must correspond to a real
    # extra_checks[...] assignment in demo_rehearsal.py.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "demo_rehearsal.py")
    with open(path) as fh:
        source = fh.read()
    for name in lib.REQUIRED_EXTRA_CHECKS:
        check(f'extra_checks["{name}"]' in source,
              f"demo_rehearsal.py actually inserts {name!r}")


def test_demo_rehearsal_passes_required_checks_to_build_demo_summary():
    # The wiring guard. build_demo_summary's `required_checks` defaults to
    # None (off) so the older unit tests above can drive it with a
    # one-element dict -- which means the whole membership gate is
    # unreachable if the real call site stops passing it. An unused
    # parameter is just a differently shaped absence.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "demo_rehearsal.py")
    with open(path) as fh:
        source = fh.read()
    check("required_checks=drlib.REQUIRED_EXTRA_CHECKS" in source,
          "demo_rehearsal.py passes required_checks=REQUIRED_EXTRA_CHECKS")
    check(source.count("drlib.build_demo_summary(") == 1,
          "exactly one build_demo_summary call site, so the guard above "
          "covers all of them")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            print(f"--- {name} ---")
            fn()

    print(f"\n{tests_passed}/{tests_run} passed, {tests_failed} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
