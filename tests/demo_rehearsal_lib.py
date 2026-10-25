#!/usr/bin/env python3
"""demo_rehearsal_lib.py -- pure logic for tests/demo_rehearsal.py (issue
#157, the demo-rehearsal harness).

Kept separate from the Playwright/subprocess-driving code in
tests/demo_rehearsal.py for the same reason ui_live_smoke_lib.py is split
out from ui_live_smoke.py: a fast, browser-free, network-free unit test
(tests/test_demo_rehearsal_lib.py) for the scheduling / conservation-check
/ verdict math, instead of burying it inside async page-driving code.
Nothing in this module touches a browser, a socket, a subprocess, or the
filesystem -- plain data in, plain data out.
"""
import math

# ── Waterfall query-latency threshold ────────────────────────────────────
#
# issue #101's own symptom is "the executions query behind Waterfall
# eventually just doesn't answer" -- there is no pre-existing stated budget
# for it (unlike ui_live_smoke_lib.FIRST_DATA_TIMEOUT_S, which times FIRST
# paint, a different question). This harness has to state one.
#
# Chosen: 10s. Reasoning, not a guess:
#   - The app's own live-tick cadence (app.js startAutoRefresh) is 5s. A
#     query that cannot beat ONE tick interval is already behind the UI by
#     the time it answers, on every subsequent tick, forever -- so 5s is
#     the point past which the panel provably falls behind, not just
#     "feels slow". 10s is exactly double that: enough headroom for a real
#     network hop + a real server under a real concurrent pgbench load
#     (never zero-latency, unlike tests/mock_server.py) without hiding a
#     genuine regression inside "it's just a slow box".
#   - It is a fifth of ui_live_smoke_lib.FIRST_DATA_TIMEOUT_S (60s): that
#     budget exists for the FIRST paint of a brand-new session and already
#     tolerates a cold daemon/bridge/websocket handshake; a warm, already-
#     connected session re-querying the same command should be held to a
#     much tighter number.
# This is the ONLY threshold in this module that governs a pass/fail
# check (see docs/VISUAL_CHECKLIST.md / CLAUDE.md "never widen a
# tolerance") -- it is not read from an env var, so a flaky box cannot
# quietly get a looser gate than the one stated here and in the report.
WATERFALL_QUERY_THRESHOLD_S = 10.0

# ── Time-model conservation tolerance ────────────────────────────────────
#
# docs/ROADMAP_AND_STATUS.md's T8 design: "CPU* + Off-CPU* + Sigma wait[c]
# = DB Time" holds BY CONSTRUCTION on the raw (per-event) compute path
# (Off-CPU* is defined as the residual that makes it hold exactly). 1% of
# db_time_ms is the tolerance: generous enough to absorb floating-point/
# ms-rounding noise.
#
# CORRECTED (owner finding, 2026-09-27): this identity is NOT a general
# dropped-class detector. src/compute.c computes Off-CPU* as the residual
# `max(0, DB_Time - CPU* - Sigma waits)` -- so if a wait class's time is
# never attributed to its own class row, that time is silently absorbed
# into Off-CPU* and the sum still closes to within this tolerance. A
# dropped wait class is INVISIBLE to this check alone (see
# the per-class wait-CPU scan in criteria section 3 / queue item 2.5 for
# the separate attribution evidence; Off-CPU magnitude cannot gate it).
#
# CORRECTED AGAIN (issue #517, 2026-10-25). This block used to end "This
# identity only catches OVER-attribution large enough to drive the residual
# negative past the clamp, or a genuinely corrupted rows array." The first
# half is wrong: that residual can never go negative, because every CPU-class
# contribution is clamped to its own interval first (src/compute.c
# `if (m > dur_ns) { cpu_clamped_ns += m - dur_ns; m = dur_ns; }`), so
# cpu_measured_ns <= Sigma CPU-class dur = db_time_ns - Sigma waits by
# construction. Over the real pgwt-server, 12 synthetic traces on the raw
# path -- 555-1340 ms of cpu_clamped_ms among them -- all reported
# gap_ms = 0.000000000. So on a RESPONSE the product can actually emit, this
# identity's gap is identically zero; the only thing it can still reject is a
# rows array corrupted in transit, plus the MIN_DB_TIME_MS floor below.
# The independently-derived replacement is class_rows_vs_top_events_ok.
TIME_MODEL_TOLERANCE_PCT = 1.0

# ── Time-model conservation: nonzero-DB-Time floor ───────────────────────
#
# Owner/reviewer finding (2026-09-27): the conservation check used to
# special-case db_time_ms <= 0 as a trivial PASS ("nothing to conserve").
# In THIS harness that is a void reading, not a clean one -- demo_rehearsal
# runs under continuous pgbench + lock/sleep load for the entire capture
# window, so a recent trailing window (or the whole capture) reporting
# ~zero DB Time means the daemon caught no real activity in that window:
# exactly the "capture with zero samples" failure mode this gate exists to
# catch, not a legitimate idle reading. MIN_DB_TIME_MS is a literal-but-
# tiny floor (not just db_time_ms > 0.0) so a value that is technically
# positive only by floating-point noise cannot slip past as "nonzero"
# either. This floor is checked BEFORE any ratio/equality below -- a gate
# that cannot see real activity must refuse, never approve on a 0 ~= 0
# reading.
MIN_DB_TIME_MS = 1.0

# ── Time-model conservation: WHICH compute path, and why it matters ──────
#
# Reviewer finding (2026-09-25, round 1): querying time_model over the
# WHOLE capture window is a VACUOUS check on every real run. This harness's
# capture window is always >= 120s, so server.c's should_use_summaries()
# always routes a whole-window query to pgwt_compute_time_model_from_
# summaries (compute.c) instead of the raw per-event path. In that
# function's tm_summary_visitor, EVERY class contribution is added to
# ctx->db_time_ns and to its own class row IN THE SAME STATEMENT
# (`ctx->classes[c].total_ns += cls_ns; ctx->db_time_ns += cls_ns;`) --
# so "class rows sum == db_time_ms" holds by construction, and no
# Off-CPU* row is ever emitted on this path. If upstream data were
# silently dropped before reaching the summary, BOTH sides would shrink
# identically and this assertion would still read PASS. It cannot go red
# for the bug class it exists to catch.
#
# Fix: ALSO query a short trailing window narrower than the 120s
# threshold, which forces pgwt_compute_time_model (the raw/exact path).
# That path sources db_time_ns independently (summed once per event, in
# the main accumulation loop, before any class attribution) and computes
# Off-CPU* as a residual (`db_time_ns - CPU* - Sigma waits`, clamped at 0)
# -- an over-attribution bug drives that residual negative and gets
# clamped, which DOES produce a detectable gap; the whole-window/summary
# path has no such residual or clamp step at all. The recent-window
# result is what gates this check's `ok`; the whole-window number is kept
# only as a labeled, non-gating diagnostic (still worth seeing, e.g. to
# spot a raw-vs-summary DISAGREEMENT, just never trusted alone).
#
# Must be strictly less than server.c's 120s should_use_summaries()
# threshold -- this is deliberately checked at runtime (demo_rehearsal.py
# asserts the response's `categories` key, raw-path-only, is actually
# present) rather than merely assumed, so a future change to that
# threshold fails this check loudly instead of silently going vacuous
# again.
RECENT_WINDOW_S = 60.0

# ── pgwt-server query timeouts ────────────────────────────────────────────
#
# Reviewer finding (round 1): tests/server_harness.py's query() blocked on
# stdout.readline() forever. A pgwt-server hang answering `executions` is
# LITERALLY the symptom the Waterfall-latency check exists to measure
# (issue #101) -- without a timeout, that exact failure mode hangs the
# whole rehearsal instead of producing a timing FAIL. Both budgets are
# generous multiples of WATERFALL_QUERY_THRESHOLD_S so a slow-but-answered
# query is always measured and graded on ITS OWN number, never mistaken
# for a hang; they exist only to bound the harness's own patience, not to
# replace the pass/fail threshold above.
EXECUTIONS_QUERY_TIMEOUT_S = 120.0
TIME_MODEL_QUERY_TIMEOUT_S = 60.0


def schedule_passes(duration_s, pass_budget_s, warmup_s=60.0,
                     tail_buffer_s=90.0, names=("early", "middle", "late")):
    """Target start offsets (seconds from t0) for len(names) tab-walk
    passes spread across [0, duration_s), so early/middle/late capture
    states are all covered (issue #157 acceptance item 2) rather than a
    single walk near one point in the window.

    warmup_s: the daemon needs a moment to have any live data at all
    (ui_live_smoke_lib.FIRST_DATA_TIMEOUT_S's same concern) -- the first
    pass never starts before this.
    tail_buffer_s: seconds reserved AFTER the last pass finishes for the
    end-of-capture checks (time-model conservation, the Waterfall query
    timing, the daemon-log scan).
    pass_budget_s: the wall-clock time ONE pass (all 11 tabs) is expected
    to take. Consecutive offsets are spaced AT LEAST pass_budget_s apart
    (never just spread across the window irrespective of how long a pass
    actually runs) -- otherwise "middle"'s target start could arrive while
    "early" is still walking tab 7, and the driver would either skip
    "middle"'s spacing or overlap two Playwright sessions against the same
    daemon. Any slack beyond the tightest back-to-back schedule is spread
    evenly, pushing every pass later (never earlier than the tight
    schedule) so the window is used, not left idle at the end.

    Returns a list of (name, offset_s) tuples. Raises ValueError if
    duration_s cannot fit warmup_s + len(names) passes back-to-back +
    tail_buffer_s at all (the caller should fail loud, never silently run
    fewer/shorter passes -- CLAUDE.md "never widen a tolerance" extends to
    never quietly shrinking the rehearsal's own coverage instead of
    reporting that it does not fit)."""
    n = len(names)
    if n == 0:
        raise ValueError("schedule_passes: need at least one pass name")
    min_span = warmup_s + n * pass_budget_s + tail_buffer_s
    if min_span > duration_s:
        raise ValueError(
            f"schedule_passes: duration_s={duration_s} cannot fit "
            f"warmup {warmup_s}s + {n} passes x {pass_budget_s}s each "
            f"(back-to-back) + tail buffer {tail_buffer_s}s "
            f"(needs >= {min_span}s) -- shorten pass_budget_s (fewer "
            "ticks/tabs) or lengthen DURATION_MIN, never silently drop "
            "coverage")
    slack = duration_s - min_span
    extra_gap = slack / n
    offsets = [warmup_s + i * pass_budget_s + (i + 1) * extra_gap
              for i in range(n)]
    return list(zip(names, offsets))


# Richest-first: a real run (default DURATION_MIN=35) always resolves to
# the first entry of each -- lib.MIN_TICKS(6) ticks/tab, all 3 passes.
# "Never lower this in a gating run" (ui_live_smoke.py's own rule for its
# --ticks flag) applies here too: only a duration far below the documented
# default -- i.e. a deliberate self-test, never a real baseline -- walks
# this list past its first entry.
DEFAULT_TICK_OPTIONS = (6, 4, 2, 1)
DEFAULT_PASS_NAME_OPTIONS = (("early", "middle", "late"),
                            ("early", "late"), ("early",))


def rehearsal_plan(duration_s):
    """Refuse an impossible baseline before starting live load (#543).

    The documented 3-minute mechanics self-test deliberately reduces coverage;
    every other invocation must fit all three passes at six ticks per tab.
    This checks the scheduling estimate, not a promise about UI wall time:
    workloads must remain alive until the caller tears them down.
    """
    if not math.isfinite(duration_s) or duration_s <= 0:
        raise ValueError("DURATION_MIN must be finite and positive")
    ticks, passes = plan_passes(duration_s)
    is_self_test = abs(duration_s - 180) < 1e-6
    if not is_self_test and (ticks < DEFAULT_TICK_OPTIONS[0] or len(passes) < 3):
        raise ValueError(
            f"{duration_s:g}s cannot fit the baseline (6 ticks/tab, 3 passes); "
            "use the default DURATION_MIN=35 or DURATION_MIN=3 for a mechanics self-test")
    return ticks, passes


def plan_passes(duration_s, tab_count=11, tick_interval_s=5.0,
                per_tab_overhead_s=5.0, tick_options=DEFAULT_TICK_OPTIONS,
                pass_name_options=DEFAULT_PASS_NAME_OPTIONS):
    """Picks the richest (ticks-per-tab, pass schedule) combination that
    fits duration_s -- preferring more ticks over more passes, since ticks
    is what ui_live_smoke_lib.build_tab_result's own ticks_ok check
    requires >= MIN_TICKS(6) of (issue #157 point (a): the self-test run,
    DURATION_MIN=3, is EXPECTED to fail ticks_ok / report fewer passes --
    that proves the harness runs end-to-end without crashing, not that it
    passes the gate; only the default 30-45 min window is a real baseline).

    per-tab budget estimate: ticks*(tick_interval_s + 2.0) + per_tab_overhead_s
    -- the "+2.0" covers ui_live_smoke.py's own fixed per-TICK settle cost
    (the 100ms blind-window shot, the anchor-to-tick+1200ms wait, the
    120ms second blink frame -- see run_tab), which is paid once per tick,
    not once per tab; per_tab_overhead_s covers the fixed once-per-tab cost
    (navigation, ready-selector wait, leak-probe settle).

    Returns (ticks, passes) where passes is schedule_passes' own return
    value. Raises ValueError if duration_s is too short to fit even the
    smallest combination (1 tick, 1 pass)."""
    # warmup_s/tail_buffer_s scale down with a short (self-test) duration_s
    # too -- schedule_passes' own 60s/90s defaults are sized for the real
    # 30-45 min run and would alone make a 3-minute self-test infeasible.
    warmup_s = min(60.0, duration_s * 0.1)
    tail_buffer_s = min(90.0, duration_s * 0.15)
    for ticks in tick_options:
        pass_budget_s = tab_count * (ticks * (tick_interval_s + 2.0) +
                                     per_tab_overhead_s)
        for names in pass_name_options:
            try:
                passes = schedule_passes(duration_s, pass_budget_s,
                                         warmup_s=warmup_s,
                                         tail_buffer_s=tail_buffer_s,
                                         names=names)
                return ticks, passes
            except ValueError:
                continue
    raise ValueError(
        f"plan_passes: duration_s={duration_s} is too short to run even "
        "one pass at the smallest tick/pass-count combination")


def time_model_conservation(rows, db_time_ms,
                            tolerance_pct=TIME_MODEL_TOLERANCE_PCT):
    """rows: the time_model response's `rows` array (list of
    {"name","ms","pct","aas","indent"} dicts). db_time_ms: the response's
    top-level `db_time_ms`.

    The class-level rows (indent == 1 -- CPU*, each wait class, and
    Off-CPU* when the raw/exact path emits it) must sum to db_time_ms
    within tolerance_pct percent (docs/ROADMAP_AND_STATUS.md's "identity
    holds by construction" -- indent 0 is the DB Time row itself, indent 2
    rows are per-event BREAKDOWNS of their indent-1 parent and would
    double-count if included). db_time_ms must ALSO clear the absolute
    MIN_DB_TIME_MS floor first -- an empty/near-empty window is void
    evidence in this harness (see MIN_DB_TIME_MS's comment), never a
    trivial pass; that floor is checked BEFORE the ratio below so a
    0 ~= 0 reading cannot satisfy this function by construction.

    Returns (ok, detail) -- detail always states the actual numbers, never
    just true/false, since a failure here IS the finding.

    #517: on the raw path this equality is an algebraic identity whose gap is
    identically 0.0 for every trace the product can produce (see
    TIME_MODEL_TOLERANCE_PCT's comment for the proof and the 12-trace
    measurement). It is kept for its MIN_DB_TIME_MS floor and as a corruption
    tripwire; the per-class attribution evidence comes from
    class_rows_vs_top_events_ok, which sums an independent endpoint."""
    if db_time_ms < MIN_DB_TIME_MS:
        return False, (
            f"db_time_ms={db_time_ms:.4f} is below the {MIN_DB_TIME_MS}ms "
            "nonzero-DB-Time floor -- void reading (no real activity "
            "captured in this window), not a passing conservation check")
    class_ms = sum(r.get("ms", 0.0) for r in rows if r.get("indent") == 1)
    gap_ms = db_time_ms - class_ms
    gap_pct = abs(gap_ms) / db_time_ms * 100.0
    ok = gap_pct <= tolerance_pct
    detail = (f"db_time_ms={db_time_ms:.1f} class_rows_sum_ms={class_ms:.1f} "
              f"gap={gap_ms:.1f}ms ({gap_pct:.2f}%, tolerance {tolerance_pct}%)")
    return ok, detail


# ── Time-model over-attribution self-checks ──────────────────────────────
#
# Owner finding (2026-09-27), the reason this whole bypass suite exists:
# time_model_conservation's identity CANNOT detect an entire wait class
# being dropped -- src/compute.c defines Off-CPU* as the residual
# `max(0, DB_Time - CPU* - Sigma waits)` (compute.c ~line 564), so time
# that a bug fails to attribute to its own class flows straight into
# Off-CPU* and the identity still closes inside 1%.
#
# CORRECTED 2026-09-27 (owner, second round): `wait_gap_cpu_ms` as a
# fraction of DB Time is NOT an over-attribution detector, and an earlier
# version of this file wrongly made it one at <=0.1%. The BPF measures
# exact on-CPU between the wait-start and wait-end writes, which spans the
# syscall's own on-CPU work, so an IO wait is LEGITIMATELY CPU-bearing -- a
# pwrite to page cache is nearly all on-CPU under a wait label.
# src/compute.h's "should be ~=0, a sleeping task burns no CPU" is a false
# premise for IO classes. Two live self-test runs measured 0.376-0.392%
# (~4k IO waits in ~40s carrying ~257ms, ~60us/event -- inside the
# syscall-overhead envelope), and that is EXPECTED on a correct
# implementation, not a defect. The a-priori bound the mechanism actually
# supports (Sigma IO-class-wait-ms + N_wait * ~10us) is honest but far too
# loose to gate on (~1.2s for that run); baselining the DB-Time fraction
# instead would be illegitimate -- it is a property of the workload's IO
# mix, not of the implementation, so it cannot separate a defect from a
# change of mix. DROPPED. The real over-attribution signature -- a
# Timeout:PgSleep or Lock:relation event (pure sleeps) carrying cpu_ns on
# the order of a millisecond, a stale wait label or an unclosed
# on_cpu_ts -- needs PER-CLASS wait CPU, which is not on the wire in the
# time_model response today (only the aggregate `wait_gap_cpu_ms` across
# ALL wait classes is). Not implemented here -- see this task's report for
# the gap; do not invent a proxy from the aggregate.
#
# `cpu_clamped_ms` is kept: nonzero means the accounting already found an
# inconsistency (CPU exceeded a gap's own wall time, or the Off-CPU*
# residual went negative and got clamped back to 0) and papered over it.
# Its scope is narrower than its name suggests: src/compute.c's wait
# branch (~line 810) never clamps `cpu_ns > dur` -- only the CPU-class
# branch does -- so `cpu_clamped_ms == 0` vouches for CPU-class gaps only,
# NEVER for wait events. 0.1% of db_time_ms is far tighter than the 1%
# identity tolerance above: it is a C-computed self-check meant to be
# exactly (or almost exactly) zero on a healthy trace, not a rounding-
# noise budget.
OVER_ATTRIBUTION_TOLERANCE_PCT = 0.1


def cpu_clamped_ok(cpu_clamped_ms, db_time_ms,
                   tolerance_pct=OVER_ATTRIBUTION_TOLERANCE_PCT):
    """cpu_clamped_ms must be within tolerance_pct percent of db_time_ms.
    Caller must have already cleared the MIN_DB_TIME_MS floor -- db_time_ms
    <=0 here raises ZeroDivisionError deliberately (never silently
    trusted). See OVER_ATTRIBUTION_TOLERANCE_PCT's comment for this
    check's narrow scope (CPU-class gaps only, never wait events)."""
    clamped_pct = abs(cpu_clamped_ms) / db_time_ms * 100.0
    ok = clamped_pct <= tolerance_pct
    detail = (f"cpu_clamped_ms={cpu_clamped_ms:.2f} ({clamped_pct:.3f}%), "
              f"tolerance {tolerance_pct}% (CPU-class gaps only -- see "
              "this check's own comment for why it cannot vouch for "
              "wait-event over-attribution)")
    return ok, detail


# Off-CPU*: deliberately REPORTED, NOT GATED (#386, 2026-10-13).
# Provenance ("SOLVED by offline decomposition"):
# https://github.com/DmitryNFomin/pg_wait_tracer/issues/386#issuecomment-6151275012
# Gate-1 retained capture, 1834s; tests/results/offcpu_decomposition/ on
# agent/offcpu-run-delay-decomposition. The settled measurement attributes
# 96.1% of Off-CPU* to _smoke_lockmgr_fanout SELECT count(*): 186 bursts,
# ~1.1s every ~10s, eight runnable backends on four shared vCPUs.
# All 1775 placements of the gate's own 60s window give min/median/p95/max
# 5.08/8.96/11.25/13.52%; 405 (22.8%) exceed the former 10% cap.
#
# Considered whole-capture bound, derived from THAT capture (not old 10%):
# burst-magnitude CV = 0.42; 6.09 bursts/window predict 0.42/sqrt(6.09)
# = 0.170 window CV (decomposition's unrounded prediction 0.169, observed
# 0.177). Over 186 bursts expected CV = 0.42/sqrt(186) = 0.0308 (3.08%).
# Using observed window CV instead gives 0.177*sqrt(6.09/186) = 0.0320.
# A candidate mean + 3*CV envelope is 8.405%*(1+3*0.0308) = 9.18% for
# that whole capture. This is a model-based contention envelope, NOT a
# correctness bound or a calibrated probability; no 9.18% cap is enforced.
# Aggregation reduces phase noise but cannot separate physical contention
# from incorrect attribution (round-2's whole-capture share is 2.779%).
# Runnable concurrency tracks contention too (passing window max 2 versus
# other windows' mean 3.33-4.50, max 7-15), not tracer correctness.
# Conclusion: neither quantity can usefully gate product correctness here;
# retain each raw window's physical ms and DB-Time percentage as diagnostics.
# The whole-capture summary path has no Off-CPU* signal; do not report its
# default zero as a physical measurement or average the sparse windows as
# though they covered the full capture.
#
# What this replacement does NOT detect: a dropped wait label moves wall
# time from a wait into a CPU gap and RAISES Off-CPU*, while the accounting
# still closes. The separate per-class wait-CPU scan (criteria / queue 2.5)
# covers attribution evidence; this diagnostic is not a dropped-label test.


def time_model_offcpu_diagnostic(offcpu_ms, db_time_ms, has_measured_cpu):
    """Report the physical residual without any pass/fail verdict.

    Legacy/sampled data and the whole-capture summary path have no measured
    Off-CPU* signal. A void DB-Time reading has no usable percentage either;
    conservation's existing floor still rejects that window independently.
    """
    measured = (has_measured_cpu is True
                and isinstance(offcpu_ms, (int, float)) and not isinstance(offcpu_ms, bool)
                and math.isfinite(offcpu_ms) and offcpu_ms >= 0)
    available = measured and db_time_ms >= MIN_DB_TIME_MS
    pct = offcpu_ms / db_time_ms * 100.0 if available else None
    if not has_measured_cpu:
        detail = "has_measured_cpu=False -- no Off-CPU* signal (unavailable, not zero)"
    elif not measured:
        detail = "Off-CPU* magnitude absent/invalid -- unavailable, not zero"
    elif not available:
        detail = "db_time_ms below the nonzero-DB-Time floor -- Off-CPU* percentage unavailable"
    else:
        detail = f"offcpu_ms={offcpu_ms:.2f} ({pct:.2f}% of db_time_ms)"
    return {
        "ok": None,
        "gating": False,
        "available": bool(available),
        "offcpu_ms": offcpu_ms if measured else None,
        "db_time_ms": db_time_ms,
        "pct": pct,
        "detail": detail + "; magnitude reported, not gated -- physical contention, not attribution correctness",
    }


def evaluate_time_model_window(rows, db_time_ms, cpu_clamped_ms,
                               offcpu_ms, has_measured_cpu, used_raw_path,
                               aas=None, check_workload_signature=False,
                               check_aas_floor=False):
    """The full per-window gate for one time_model response: the identity
    conservation check (with its own nonzero-DB-Time floor), the raw/exact
    compute path requirement, the cpu_clamped_ms self-check (CPU-class
    gaps only -- see its own comment), and (when
    requested) the criteria-doc #2 workload signature -- both required
    events present with real time. ALL
    requested checks must hold, checked in that order (raw floors before
    any ratio/equality, per CLAUDE.md/issue #157's bypass-suite
    requirement). Used both for the repeated in-capture samples (gating --
    see conservation_sample_interval_s, which also requests the signature
    and AAS diagnostic) and for the one-off whole-capture-window diagnostic (never
    gating -- see RECENT_WINDOW_S's comment; the caller leaves
    check_workload_signature/check_aas_floor False there since a 35-45 min
    whole-window AAS average is not "the 60s recent window" the criteria
    doc reports). The legacy check_aas_floor flag now requests reported-only
    AAS, with ok=None and gating=False; it cannot change the window verdict.
    Off-CPU* is retained as a magnitude diagnostic, with ok=None. Its
    availability is required; no physical magnitude bound is registered.

    NOTE (owner correction, 2026-09-27): this used to also gate on
    `wait_gap_cpu_ms <= 0.1% of db_time_ms` -- dropped entirely, see
    OVER_ATTRIBUTION_TOLERANCE_PCT's comment for why that was not a valid
    over-attribution detector at all (IO waits are legitimately CPU-
    bearing). The real per-class signature this check should use instead
    is not on the wire and is NOT implemented here (see this task's
    report)."""
    cons_ok, cons_detail = time_model_conservation(rows, db_time_ms)
    if db_time_ms >= MIN_DB_TIME_MS:
        clamped_ok, clamped_detail = cpu_clamped_ok(cpu_clamped_ms, db_time_ms)
    else:
        clamped_ok, clamped_detail = False, (
            "db_time_ms below the nonzero-DB-Time floor -- cpu_clamped_ms "
            "check skipped, not vacuously ok")
    offcpu = time_model_offcpu_diagnostic(offcpu_ms, db_time_ms, has_measured_cpu)
    # No physical magnitude cap is registered (§3): only signal availability
    # gates. Legacy/summary absence must never count as a measured zero.
    ok = bool(cons_ok and used_raw_path and clamped_ok and offcpu["available"])
    result = {
        "ok": ok,
        "compute_path": "raw" if used_raw_path else "summary",
        "conservation": {"ok": cons_ok, "detail": cons_detail},
        "cpu_clamped": {"ok": clamped_ok, "detail": clamped_detail},
        "offcpu": offcpu,
    }
    if check_workload_signature:
        sig_ok, sig_detail = workload_signature_present_ok(rows)
        result["ok"] = bool(result["ok"] and sig_ok)
        result["workload_signature"] = {"ok": sig_ok, "detail": sig_detail}
    if check_aas_floor:
        aas_ok, aas_detail = aas_floor_ok(aas)
        result["aas_floor"] = {
            "ok": aas_ok, "gating": False, "aas": aas, "detail": aas_detail,
        }
    return result


# ── Conservation sampled THROUGHOUT the capture, not just at the end ─────
#
# Owner finding (2026-09-27): the checks above used to run against a single
# trailing window at the very end of a 35-minute run -- n=1. "The audience
# watches the whole run, not the last minute of it": a regression visible
# for only part of the window could sit entirely outside that one sample.
CONSERVATION_TARGET_INTERVAL_S = 300.0  # 5 minutes, a real 30-45 min run


def conservation_sample_interval_s(duration_s,
                                   target_interval_s=CONSERVATION_TARGET_INTERVAL_S,
                                   min_samples=3):
    """How often (seconds) to re-run the time-model gate through the whole
    capture. target_interval_s (default 300 = 5 min) is what a REAL run
    gets; scaled down for a short self-test (DURATION_MIN=3) so it still
    exercises >= min_samples -- the same "never silently drop coverage"
    rule schedule_passes/plan_passes already follow, extended to this
    check's own coverage of time, not just of tabs."""
    if duration_s <= 0:
        raise ValueError(
            f"conservation_sample_interval_s: duration_s={duration_s} must be positive")
    return max(min(target_interval_s, duration_s / min_samples), 1.0)


def build_demo_conservation_check(samples, full_window_result):
    """samples: list of {"offset_s":..., **evaluate_time_model_window(...)}
    dicts, one per in-capture sample. EVERY sample must pass -- not an
    average, not just the last one (see CONSERVATION_TARGET_INTERVAL_S's
    comment). An empty samples list is void (no evidence was ever
    gathered through the run) and is NEVER vacuously ok, regardless of
    what full_window_result says. full_window_result is the whole-capture-
    window query, kept purely as a labeled, non-gating diagnostic (see
    RECENT_WINDOW_S / evaluate_time_model_window's docstring)."""
    def sample_ok(sample):
        return sample.get("ok") and sample.get("offcpu", {}).get("available") is True

    ok = len(samples) > 0 and all(sample_ok(s) for s in samples)
    failed_offsets = [round(s.get("offset_s", -1), 1) for s in samples
                      if not sample_ok(s)]
    return {
        "ok": ok,
        "num_samples": len(samples),
        "failed_offsets_s": failed_offsets,
        "samples": samples,
        "full_window": dict(
            full_window_result,
            note=("informational only -- does not gate `ok`; the whole-"
                  "capture window is always long enough to route to the "
                  "summary compute path (src/compute.c tm_summary_visitor), "
                  "which cannot detect under- or over-attribution -- see "
                  "RECENT_WINDOW_S")),
    }


def waterfall_latency_ok(elapsed_s, threshold_s=WATERFALL_QUERY_THRESHOLD_S):
    return elapsed_s is not None and elapsed_s <= threshold_s


# ── docs/DEMO_REHEARSAL_CRITERIA.md floors ────────────────────────────────
#
# Owner-pre-registered criteria (2026-09-27, agent/rehearsal-criteria commit
# 7beeb28), section 2: two more raw floors, checked before any ratio, that
# this harness did not measure at all before this change.

# tests/live_loop_workload.py's holder/waiter loop and adv_holder/reporter
# pair create BOTH of these events, by construction, on every iteration --
# their absence from a window means this window captured something other
# than the intended workload (a dead workload process, a misrouted window,
# a misclassified event), not a legitimately quiet window. Full
# "Class:Event" names, exactly as src/wait_event.c pgwt_event_full_name
# emits them into a time_model response's sub-event (indent==2) rows.
#
# Timeout:PgSleep was in this pair until 2026-10-06, when the DEMO workload
# dropped pg_sleep entirely (owner: a literal `pg_sleep(1.3)` visible in the
# Top Queries panel at PGCONF.EU reads as a faked slow query to any DBA in
# the room). pg_sleep itself is untouched as the accuracy tests' known
# quantity (test_aas_accuracy.py, test_accuracy.py, test_deterministic.py,
# test_query_accuracy.py, test_capture_smoke.py default-arg call sites) --
# only the live demo loop changed. Lock:advisory (adv_holder holding
# pg_advisory_lock(42) for a client-side sleep while the reporter's own
# pg_advisory_lock(42) call blocks on it) replaces it as the second
# required event, derived the same way: it is what this loop now produces
# unconditionally every iteration, not a floor chosen independently of the
# workload.
REQUIRED_WORKLOAD_EVENTS = ("Lock:relation", "Lock:advisory")


def workload_signature_present_ok(rows, required=REQUIRED_WORKLOAD_EVENTS):
    """Both of `required` must appear in `rows` with ms > 0. A time_model
    response's sub-event rows only carry the top 5 per class (src/compute.c)
    -- if the workload is running as intended this pair dominates its own
    class, so this is a floor on "is the intended workload even present",
    not a coverage guarantee for every possible event."""
    seen_ms = {name: 0.0 for name in required}
    for r in rows:
        name = r.get("name")
        if name in seen_ms:
            seen_ms[name] = max(seen_ms[name], r.get("ms", 0.0) or 0.0)
    missing = [name for name in required if seen_ms[name] <= 0]
    ok = len(missing) == 0
    detail = ", ".join(f"{name}={seen_ms[name]:.1f}ms" for name in required)
    if missing:
        detail += f" -- MISSING/zero: {', '.join(missing)}"
    return ok, detail


# WITHDRAWN (owner-approved criteria amendment, 2026-10-24, §2): the floor's
# only stated purpose was anti-vacuity. Was AAS_FLOOR_PROVISIONAL = 0.5
# (non-vacuous-only: reject an AAS reading of ~0 the same way MIN_DB_TIME_MS
# rejects a ~0 db_time_ms). MIN_DB_TIME_MS, capture_has_events_ok and
# workload_signature_present_ok already close that hazard. Raising it to
# 1.15 added no new hazard: it was half of derived=2.3033 (run.id 1790869219,
# n=1, on a now-deleted throwaway VM). AAS also measures box response speed;
# applied-load qualification is planned, not built. Retain the historical
# constant/call interface for diagnostics, never as a verdict threshold.
AAS_FLOOR = 1.15


def aas_floor_ok(aas, floor=AAS_FLOOR):
    """Legacy interface: report AAS without a pass/fail verdict, like Off-CPU*."""
    detail = f"aas={aas!r} (withdrawn floor {floor}); reported, not gated"
    return None, detail


# ── Cross-tab agreement and freshness (criteria doc §5) ──────────────────
#
# Two endpoints answering queries for the SAME window must report the same
# DB Time within TIME_MODEL_TOLERANCE_PCT -- a disagreeing denominator
# between tabs is the product-facing version of a bookkeeping error. Uses
# the SAME 1% bound as the identity check (docs/DEMO_REHEARSAL_CRITERIA.md
# section 5 pins it to TIME_MODEL_TOLERANCE_PCT explicitly, not a separate
# number).
def cross_tab_db_time_agreement_ok(db_time_a, db_time_b,
                                   tolerance_pct=TIME_MODEL_TOLERANCE_PCT):
    if db_time_a is None or db_time_b is None:
        return False, f"missing db_time_ms (a={db_time_a!r}, b={db_time_b!r})"
    if db_time_a < MIN_DB_TIME_MS or db_time_b < MIN_DB_TIME_MS:
        return False, (f"one side is below the nonzero-DB-Time floor "
                       f"(a={db_time_a:.2f}ms, b={db_time_b:.2f}ms)")
    gap_pct = abs(db_time_a - db_time_b) / max(db_time_a, db_time_b) * 100.0
    ok = gap_pct <= tolerance_pct
    detail = (f"a={db_time_a:.1f}ms b={db_time_b:.1f}ms gap={gap_pct:.2f}% "
              f"(tolerance {tolerance_pct}%)")
    return ok, detail


# ── Per-class attribution, re-derived from an INDEPENDENT endpoint ───────
#
# Issue #517, settled by measurement 2026-10-25. time_model_conservation
# above compares db_time_ms against the sum of the SAME response's indent-1
# rows, and on the raw/exact path that comparison is an ALGEBRAIC IDENTITY
# whose gap is exactly zero for every possible trace -- it is not merely
# "zero on the runs we have looked at".
#
# The proof, from src/compute.c's raw path (pgwt_compute_time_model):
#   db_time_ns      = Sigma dur over non-idle, non-io_worker events (line ~1404)
#   classes[c!=CPU] = Sigma dur over that class's events  (line ~1444)
#   classes[CPU]    = cpu_measured_ns                     (line ~1453)
#   offcpu_ns       = max(0, db_time_ns - cpu_measured_ns - Sigma waits) (~1470)
# so Sigma(indent-1 rows) = cpu_measured + Sigma waits + offcpu = db_time_ns
# whenever the residual is non-negative. And the residual CANNOT be
# negative: every CPU-class contribution is clamped to its own interval
# (`if (m > dur_ns) { cpu_clamped_ns += m - dur_ns; m = dur_ns; }`), so
# cpu_measured_ns <= Sigma CPU-class dur = db_time_ns - Sigma waits. The
# clamp branch at ~1470 is therefore unreachable from the raw path, and the
# printed gap is identically 0.0 ms.
#
# MEASURED (not inferred): 12 synthetic traces through the real pgwt-server
# on the raw path -- including traces where cpu_ns deliberately exceeds its
# interval, which drove cpu_clamped_ms to 555-1340 ms -- gave gap_ms =
# 0.000000000 on all 12. See this task's report for the probe.
#
# What that leaves time_model_conservation worth: its MIN_DB_TIME_MS floor
# (a void window must refuse, not approve) and its raw-path requirement.
# As a conservation DETECTOR it is spent: a wait class whose time is never
# attributed to its own class row flows into the Off-CPU* residual and the
# identity still closes to 0.0 (the file already says this; the new part is
# that there is no OTHER input that makes it close to anything else).
#
# So this check re-derives the per-class split from a DIFFERENT endpoint
# with a DIFFERENT accumulator: top_events (pgwt_compute_top_events) builds
# its own per-event hash table over the same window and labels each row with
# pgwt_class_name(event_id), while time_model attributes via
# pgwt_wait_class_index(old_event). Regrouping top_events' rows by `class`
# and comparing against time_model's indent-1 rows therefore compares two
# independent sums, and a class that time_model fails to attribute shows up
# as a per-class disagreement even though both responses' db_time_ms agree.
#
# MEASURED on the same 12 traces: every wait class agreed to the last
# printed digit, and time_model's CPU pair (CPU (running) + CPU (waiting
# for a core)) equalled top_events' CPU row exactly -- so a 1% per-class
# bound is a correctness bound here, not a noise budget.
#
# LIMIT, stated rather than hidden: both endpoints read the same trace
# through the same pgwt_filter_matches / pgwt_is_idle_event chokepoints, so a
# loss upstream of the server shrinks both sides together and is invisible
# here -- as it is to any two-endpoint comparison. capture_has_events,
# workload_signature_present_ok and the MIN_DB_TIME_MS floors are what cover
# that direction.

# time_model's indent-1 class-row names are src/compute.h's
# pgwt_class_display values, except the CPU group, which compute.c renders
# as two rows ("CPU (running)" + "CPU (waiting for a core)") where the
# measured split exists and as a single "CPU*" row where it does not. All
# three fold onto top_events' own "CPU" class label.
TIME_MODEL_CPU_ROW_NAMES = ("CPU (running)", "CPU (waiting for a core)", "CPU*")

# src/compute.h pgwt_class_display[] == src/wait_event.c pgwt_class_name()'s
# return set, verified identical 2026-10-25. An indent-1 row name or a
# top_events `class` value outside this set means one of the two sides grew a
# label this check does not understand: it REFUSES rather than silently
# dropping that class's time out of the comparison (which is how a gate
# stops seeing the thing it was written to see).
WAIT_CLASS_LABELS = ("CPU", "IO", "Lock", "LWLock", "IPC", "Client",
                     "Timeout", "BufferPin", "Activity", "Extension",
                     "Unknown")


def _finite_number(v):
    return (isinstance(v, (int, float)) and not isinstance(v, bool)
            and math.isfinite(v))


def class_rows_vs_top_events_ok(tm_rows, top_events_rows,
                                top_events_db_time_ms,
                                tolerance_pct=TIME_MODEL_TOLERANCE_PCT):
    """Compare time_model's per-class split against the SAME window's
    top_events rows regrouped by their own `class` field -- two independent
    accumulations, unlike time_model_conservation's identity (see the block
    comment above).

    tm_rows: time_model's `rows` array. top_events_rows: top_events' `rows`
    array (each row carries "class", "total_ms" and "pct"; src/server.c
    emits `pct` as null exactly for the idle-but-visible events its own
    db_time_ms excludes -- src/compute.c sets PGWT_PCT_DB_IDLE under the
    same pgwt_is_idle_event() predicate -- so a null `pct` is the server's
    own "not in DB Time" marker, not a guess).
    top_events_db_time_ms: that response's own top-level db_time_ms, used as
    a TRUST PRECONDITION (see below), never as the comparison.

    Fails closed -- returns (False, why) -- on anything it cannot evaluate:
    either side empty, a missing/unparseable class label or total_ms, a
    `pct` key absent (idle cannot be told from non-idle), either side below
    MIN_DB_TIME_MS, or top_events' non-idle rows not summing to its own
    db_time_ms (which is what a truncated row list looks like; a truncated
    list would make every class look smaller and the comparison meaningless).
    """
    if not tm_rows:
        return False, "no time_model rows -- void, not a passing comparison"
    if not top_events_rows:
        return False, "no top_events rows -- void, not a passing comparison"

    tm_by_class = {}
    tm_indent1 = 0
    for r in tm_rows:
        if not isinstance(r, dict) or r.get("indent") != 1:
            continue
        tm_indent1 += 1
        name = r.get("name")
        ms = r.get("ms")
        if not _finite_number(ms):
            return False, (f"time_model row {name!r} has unparseable ms "
                           f"({ms!r}) -- refusing, not skipping it")
        label = "CPU" if name in TIME_MODEL_CPU_ROW_NAMES else name
        if label not in WAIT_CLASS_LABELS:
            return False, (f"time_model indent-1 row name {name!r} is not a "
                           "known class label -- refusing (an unknown label "
                           "would otherwise drop that class's time out of "
                           "this comparison)")
        tm_by_class[label] = tm_by_class.get(label, 0.0) + ms
    if tm_indent1 == 0:
        return False, ("time_model returned no indent-1 class rows -- void, "
                       "not a passing comparison")

    te_by_class = {}
    te_non_idle_sum = 0.0
    te_non_idle_rows = 0
    for r in top_events_rows:
        if not isinstance(r, dict):
            return False, f"top_events row is not an object ({r!r})"
        if "pct" not in r:
            return False, ("a top_events row has no `pct` key -- idle-but-"
                           "visible rows cannot be told from DB-Time rows, "
                           "refusing")
        label = r.get("class")
        ms = r.get("total_ms")
        if not isinstance(label, str) or not label:
            return False, (f"top_events row {r.get('name')!r} has no usable "
                           f"`class` ({label!r}) -- refusing")
        if not _finite_number(ms):
            return False, (f"top_events row {r.get('name')!r} has unparseable "
                           f"total_ms ({ms!r}) -- refusing")
        if r["pct"] is None:
            continue            # idle-but-visible: outside DB Time by design
        if label not in WAIT_CLASS_LABELS:
            return False, (f"top_events row {r.get('name')!r} reports class "
                           f"{label!r}, which is not a known class label -- "
                           "refusing")
        te_by_class[label] = te_by_class.get(label, 0.0) + ms
        te_non_idle_sum += ms
        te_non_idle_rows += 1
    if te_non_idle_rows == 0:
        return False, ("top_events returned no DB-Time rows (every row was "
                       "idle-but-visible) -- void, not a passing comparison")

    # Trust precondition, not the detection: top_events' own db_time_ms is
    # accumulated over the same records its rows are, so its non-idle rows
    # must add up to it. They do not if the row list was truncated, which
    # would shrink every class on one side and make the per-class comparison
    # below silently meaningless.
    if not _finite_number(top_events_db_time_ms):
        return False, (f"top_events db_time_ms is unparseable "
                       f"({top_events_db_time_ms!r}) -- cannot verify its row "
                       "list is complete, refusing")
    if top_events_db_time_ms < MIN_DB_TIME_MS:
        return False, (f"top_events db_time_ms={top_events_db_time_ms:.4f} is "
                       f"below the {MIN_DB_TIME_MS}ms nonzero-DB-Time floor -- "
                       "void reading, not a passing comparison")
    rowsum_gap_pct = (abs(te_non_idle_sum - top_events_db_time_ms)
                      / top_events_db_time_ms * 100.0)
    if rowsum_gap_pct > tolerance_pct:
        return False, (f"top_events' DB-Time rows sum to "
                       f"{te_non_idle_sum:.1f}ms but it reports "
                       f"db_time_ms={top_events_db_time_ms:.1f}ms "
                       f"({rowsum_gap_pct:.2f}%) -- its row list is "
                       "incomplete, so the per-class comparison cannot be "
                       "trusted; refusing")

    tm_total = sum(tm_by_class.values())
    if tm_total < MIN_DB_TIME_MS:
        return False, (f"time_model class rows sum to {tm_total:.4f}ms, below "
                       f"the {MIN_DB_TIME_MS}ms nonzero-DB-Time floor -- void "
                       "reading, not a passing comparison")

    # The UNION, so a class present on one side and absent on the other is a
    # FAILURE rather than something the loop never visits. "Absent" is the
    # shape the bug this check exists for actually takes.
    failures = []
    parts = []
    worst_pct = 0.0
    for label in sorted(set(tm_by_class) | set(te_by_class)):
        a = tm_by_class.get(label, 0.0)
        b = te_by_class.get(label, 0.0)
        denom = max(abs(a), abs(b))
        gap_pct = 0.0 if denom == 0 else abs(a - b) / denom * 100.0
        worst_pct = max(worst_pct, gap_pct)
        parts.append(f"{label} tm={a:.1f}/te={b:.1f} ({gap_pct:.2f}%)")
        if gap_pct > tolerance_pct:
            failures.append(f"{label}: time_model={a:.1f}ms vs "
                            f"top_events={b:.1f}ms ({gap_pct:.2f}%)")
    ok = not failures
    detail = (f"{len(parts)} classes, worst gap {worst_pct:.2f}% "
              f"(tolerance {tolerance_pct}%): " + "; ".join(parts))
    if failures:
        detail += " -- DISAGREES: " + "; ".join(failures)
    return ok, detail


# ── Cross-tab AAS agreement, bucket-weighted (criteria doc §5, item 2.2) ──
#
# A previous walk reported 0.0000% from an UNWEIGHTED MEAN of the `aas`
# endpoint's buckets -- docs/DEMO_REHEARSAL_CRITERIA.md records that as
# establishing no agreement, because it is not the same quantity as
# time_model's top-level `aas` (db_time_ms / wall_ms over the window).
#
# Why a plain mean of buckets is the wrong comparison: every bucket's `aas`
# value already comes normalized by the endpoint's own (single, constant
# per response) `bucket_ns` -- src/compute.c accumulates busy-ns per bucket
# and divides by bucket_ns before returning it. `aas_i * bucket_ns` recovers
# that bucket's raw accumulated ns exactly, INCLUDING for a truncated tail
# bucket: `bucket_ns` is a CEILING of range_ns/num_buckets
# (src/compute.c: `(range_ns + num_buckets - 1) / num_buckets`), so
# `actual_buckets * bucket_ns` can exceed the true window (`to_ns - from_ns`)
# -- the server still divides that last bucket's partial accumulation by
# the FULL bucket_ns.
#
# CORRECTED (issue #517, 2026-10-25). These lines used to read "by up to
# one bucket's width ... Wall-clock windows essentially never divide
# bucket_ns evenly, so this truncation is the common case, not an edge
# case." The magnitude claim is wrong, and the issue's proposed remedy
# ("request a bucket count that does not divide the window evenly")
# follows from it and cannot work. With bucket_ns = ceil(range_ns/N) the
# overshoot is `N*bucket_ns - range_ns`, which is at most N-1
# NANOSECONDS -- not a bucket width -- so the ratio between a
# bucket-weighted and an unweighted derivation is bounded by
# 1 + (N-1)/range_ns. MEASURED on the real pgwt-server over 16
# (window, bucket-count) geometries, including 7/59/60/61/600 buckets and
# windows of 12.3/30/45.1/59.5/60/60.5/90.1 s: the worst
# |weighted/unweighted - 1| was 9.11e-7 %, nine orders of magnitude below
# TIME_MODEL_TOLERANCE_PCT. The 60-buckets-over-60s "degenerate"
# configuration is therefore not special; NO raw-path geometry makes the
# weighted-vs-unweighted distinction reach this check's tolerance.
#
# So do not change AAS_CROSS_TAB_NUM_BUCKETS hoping to exercise that
# distinction. What this check really gates -- and what IS able to go red,
# see test_bucket_weighted_aas_ok_catches_an_endpoint_level_divergence --
# is the CROSS-ENDPOINT comparison: the `aas` endpoint
# (pgwt_compute_aas: per-bucket clipping, normalised by bucket_ns) against
# time_model's own top-level `aas` (db_time_ms/wall_ms), two separate
# implementations of the same quantity over the identical window. The
# weighted form stays because it is the ARITHMETICALLY CORRECT
# re-derivation, not because the alternative is detectable here.
#
# The correct re-derivation: recover each bucket's raw ns (aas_i *
# bucket_ns), sum them, and divide by the TRUE window length (to_ns -
# from_ns, never actual_buckets * bucket_ns) -- this is exactly the ratio
# time_model's own `aas` is computed as, over the identical window. An
# UNWEIGHTED mean instead divides by bucket COUNT, silently treating every
# bucket -- including a truncated tail one -- as an equal, full-width
# sample. See test_bucket_weighted_aas_ok_catches_what_unweighted_mean_would_miss
# for a case where the two formulas diverge enough to flip pass/fail.
def _bucket_total_aas(bucket):
    """Sum every additive numeric field in one `aas` endpoint bucket
    (src/compute.h's pgwt_class_names[0..PGWT_NUM_CLASSES) plus the
    additive T8 "offcpu" field) -- summing by key membership rather than
    hard-coding compute.h's class-name list means a future additive class
    needs no change here. Excludes "t" (the bucket's start_ns, not an AAS
    value) and "cat" (a nested per-category breakdown that DOUBLE-COUNTS
    against the class total already summed -- src/server.c's own comment:
    "io_worker appears ONLY here -- excluded from the class AAS above")."""
    total = 0.0
    for k, v in bucket.items():
        if k in ("t", "cat"):
            continue
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            continue
        total += v
    return total


def bucket_weighted_aas_ok(buckets, bucket_ns, from_ns, to_ns, time_model_aas,
                           tolerance_pct=TIME_MODEL_TOLERANCE_PCT):
    """buckets: the `aas` endpoint's own `buckets` array (list of dicts with
    "t" plus per-class/offcpu numeric fields) for [from_ns, to_ns).
    bucket_ns: the SAME response's top-level `bucket_ns` (the endpoint's own
    bucket width -- the "weights" this check is required to derive them
    from). time_model_aas: `time_model`'s top-level `aas` for the IDENTICAL
    [from_ns, to_ns) window. Returns (ok, detail); fails closed on anything
    it cannot evaluate (empty buckets, missing/non-positive bucket_ns,
    missing/non-numeric time_model_aas, non-positive window) rather than
    silently passing."""
    if not buckets:
        return False, "no buckets in aas response"
    if bucket_ns is None or not isinstance(bucket_ns, (int, float)) or bucket_ns <= 0:
        return False, f"missing/invalid bucket_ns ({bucket_ns!r})"
    if (time_model_aas is None or isinstance(time_model_aas, bool)
            or not isinstance(time_model_aas, (int, float))):
        return False, f"missing/invalid time_model aas ({time_model_aas!r})"
    if from_ns is None or to_ns is None or to_ns <= from_ns:
        return False, f"non-positive window (from_ns={from_ns!r}, to_ns={to_ns!r})"
    range_ns = to_ns - from_ns
    weighted_ns_sum = sum(_bucket_total_aas(b) * bucket_ns for b in buckets)
    derived_aas = weighted_ns_sum / range_ns
    denom = max(abs(derived_aas), abs(time_model_aas))
    gap_pct = 0.0 if denom == 0 else abs(derived_aas - time_model_aas) / denom * 100.0
    ok = gap_pct <= tolerance_pct
    detail = (f"derived={derived_aas:.4f} time_model={time_model_aas:.4f} "
              f"gap={gap_pct:.2f}% (tolerance {tolerance_pct}%, "
              f"{len(buckets)} buckets x {bucket_ns / 1e6:.1f}ms, "
              f"window={range_ns / 1e9:.1f}s)")
    return ok, detail


# 5s: web/static's own live-tick cadence (app.js startAutoRefresh, the same
# reference RECENT_WINDOW_S's comment above uses). "Within 2 ticks of wall
# clock" (criteria doc §5) is 10s.
FRESHNESS_TICK_S = 5.0
FRESHNESS_MAX_TICKS = 2


def freshness_ok(now_ns, to_ns, tick_s=FRESHNESS_TICK_S,
                 max_ticks=FRESHNESS_MAX_TICKS):
    """now_ns/to_ns both come from the SAME pgwt-server `info` response
    (src/server.c handle_info emits both the daemon's latest captured event
    time and the server's own wall clock together) -- no cross-machine
    clock-skew risk from comparing a value stamped on one host against a
    clock read on another. A capture that stalled (daemon alive but no
    longer receiving events) would have to_ns stop advancing while now_ns
    keeps moving, growing this gap without bound."""
    if now_ns is None or to_ns is None:
        return False, f"now_ns/to_ns missing from the info response (now_ns={now_ns!r}, to_ns={to_ns!r})"
    age_s = (now_ns - to_ns) / 1e9
    bound_s = tick_s * max_ticks
    ok = age_s <= bound_s
    detail = f"age={age_s:.1f}s (bound {bound_s:.0f}s = {max_ticks} ticks x {tick_s:.0f}s)"
    return ok, detail


# ── Blink-sweep offset coverage (owner finding, 2026-09-28) ──────────────
#
# A live rehearsal run (run.id 1790574871, 19a95f7) showed the offset
# sweep's samples landing FAR past their targets when a tab's mount was
# late -- e.g. the 200ms-target first sample actually captured at 669,
# 985, 2416, even 2446ms. When that happens the sweep silently SKIPS the
# early window entirely, which is exactly where scatter's and transitions'
# real transients live (0.1305 and 0.0072-0.0172 respectively, both at the
# 200->500ms pair, invisible to a sweep whose first sample never got near
# 200ms). Drift remains reported; absent early-window coverage now
# refuses a rehearsal. Scheduling is unchanged. Un-caught coverage loss
# here would be the NEXT version of
# the same blindness this whole bypass suite exists to close: a gate that
# silently never looks at the interval where the defect lives.
def sweep_offset_drift(tick_record):
    """tick_record: one ui_live_smoke_lib.build_sweep_tick_record() dict
    (target_offsets_ms, achieved_offsets_ms, same length/order -- already
    present in every tab result's blink_sweep.ticks, no new instrumentation
    needed). Returns a dict with per-offset drift_ms (achieved - target)
    and the first (200ms target) offset's own drift, since that is where
    this finding's transients live.

    capture_ms/capture_ms_total_ms/panel_dims (issue #252 secondary
    finding): build_sweep_tick_record already measures these, but this
    function used to drop them when re-deriving its own per-tick view --
    the ONE place `sweep_offset_coverage` (this function's caller) is meant
    to make drift legible also had no cost data next to it, so a reader
    checking a drifting tick here saw nothing that would attribute the
    drift to capture cost, even though the raw number existed a few keys
    away in the same summary.json under blink_sweep.ticks. Carried through
    unchanged, same emptiness semantics as the source field."""
    targets = tick_record.get("target_offsets_ms") or []
    achieved = tick_record.get("achieved_offsets_ms") or []
    n = min(len(targets), len(achieved))
    drift_ms = [achieved[i] - targets[i] for i in range(n)]
    first_drift_ms = drift_ms[0] if drift_ms else None
    first_target_ms = targets[0] if targets else None
    first_achieved_ms = achieved[0] if achieved else None
    return {
        "target_offsets_ms": list(targets),
        "achieved_offsets_ms": list(achieved),
        "drift_ms": drift_ms,
        "first_target_ms": first_target_ms,
        "first_achieved_ms": first_achieved_ms,
        "first_drift_ms": first_drift_ms,
        "capture_ms": list(tick_record.get("capture_ms") or []),
        "capture_ms_total_ms": tick_record.get("capture_ms_total_ms"),
        "panel_dims": dict(tick_record.get("panel_dims") or {}),
    }


def summarize_sweep_offset_coverage(tab_id, pass_name, blink_sweep_ticks,
                                    second_target_ms):
    """Require early-window evidence from at least one tick per tab sweep.

    500 ms is the existing second sweep target, not a new drift tolerance.
    A first sample at/past it never observed that window. Refuse only when
    the tab has no valid first sample before it; retain every tick's miss
    in detail. Other drift stays reporting-only, with no new bound.
    """
    ticks = [sweep_offset_drift(t) for t in (blink_sweep_ticks or [])]
    misses = []
    early_window_observed = False
    # #397 tracks the scheduling tail: isolated late ticks must remain
    # visible, but per-tick strictness is deferred because only per-tab
    # absence is authorised, not an every-tick requirement or pass rate.
    if not ticks:
        misses.append(f"{pass_name}/{tab_id}: no sweep samples -- early window absent")
    for i, tick in enumerate(ticks, start=1):
        first = tick["first_achieved_ms"]
        label = f"{pass_name}/{tab_id} tick {i}: first sample={first!r}ms"
        if (isinstance(first, bool) or not isinstance(first, (int, float))
                or not math.isfinite(first) or first < 0):
            misses.append(label + " -- early-window measurement absent/invalid")
        elif first >= second_target_ms:
            misses.append(label + " -- EARLY WINDOW LIKELY MISSED "
                          f"(at/past {second_target_ms}ms)")
        else:
            early_window_observed = True
    return {
        "tab": tab_id,
        "pass": pass_name,
        "ticks": ticks,
        "ok": early_window_observed,
        "detail": "; ".join(misses) if misses else None,
    }


# ── Daemon integrity (criteria doc §6) ────────────────────────────────────
#
# "The daemon was alive" (daemon_log_clean, _assert_daemon_alive) is not
# "the daemon captured everything". These three counters are already in
# the daemon's own metrics blob (src/control.c, the "metrics" control
# command), reachable through pgwt-server's control proxy
# (`{"cmd":"control","request":{"cmd":"metrics"}}`, src/server.c
# handle_control) -- the SAME mechanism web/static/lib/control.js's
# controlMetrics() already uses from the UI, so no new src/ instrumentation
# is needed here.
DAEMON_INTEGRITY_COUNTERS = ("ringbuf_drops_total", "state_map_full_total",
                             "seen_query_ids_full_total")


def daemon_integrity_ok(metrics):
    """All three of DAEMON_INTEGRITY_COUNTERS must be exactly 0.
    ringbuf_drops_total is the full tier's BPF-side event_ringbuf drop
    count -- trace events are what DB Time is built from, so a nonzero
    value here means data conservation was already violated upstream of
    every other check in this file. state_map_full_total /
    seen_query_ids_full_total are BPF/userspace insert-failure counters
    (a backend recording nothing, or losing query attribution).

    A missing or non-numeric counter FAILS (a gate that cannot see the
    count must refuse), same as capture_has_events_ok's own contract.

    KNOWN BLIND SPOT, stated rather than implied (criteria doc §6): a lost
    LIFECYCLE event is silent -- lifecycle_rb reserve failures increment no
    counter, so the symptom is a backend simply absent from the capture,
    never a nonzero counter here. A clean result means "no TRACE events
    were dropped", NEVER "nothing was missed" -- closing that blind spot
    needs src/ work, out of scope for this branch."""
    if not isinstance(metrics, dict):
        return False, f"metrics response is not a dict: {metrics!r}"
    bad = []
    parts = []
    for name in DAEMON_INTEGRITY_COUNTERS:
        v = metrics.get(name)
        parts.append(f"{name}={v!r}")
        if isinstance(v, bool) or not isinstance(v, (int, float)) or v != 0:
            bad.append(f"{name}={v!r}")
    ok = len(bad) == 0
    detail = ", ".join(parts)
    if bad:
        detail += f" -- NONZERO or missing: {', '.join(bad)}"
    return ok, detail


def cmd_markers_observed(metrics):
    """#222 review: whether this capture's daemon ever emitted a CMD_START/
    CMD_END marker pair -- the ONLY thing that closes an orphaned execution
    row (src/compute.c's pgwt_compute_executions, CMD_END branch) for a
    statement that errored, was cancelled, hit statement_timeout, or lost
    its EXEC_END to a ringbuf drop. CMD markers are gated on
    `st->wp_live && exact_admission_open()` (src/bpf/pg_wait_tracer.bpf.c) --
    a pid that never escalates to a live watchpoint during the whole
    capture produces none, and for that pid the orphan-close fix is
    INERT: a single presenter Ctrl-C on that connection still pins one
    "In progress" row at the top of the Waterfall tab for the rest of the
    session, same as before #222's review round.

    Informational only -- does NOT gate `ok`. Whether a rehearsal's
    workload is expected to escalate any backend to exact tier is a
    product/workload question this harness does not decide; this just
    makes the fact visible instead of silently reading zero in a metrics
    blob nobody diffs. See also DAEMON_INTEGRITY_COUNTERS above (a
    related but distinct question: dropped vs. never-emitted markers)."""
    if not isinstance(metrics, dict):
        return False, 0, f"metrics response is not a dict: {metrics!r}"
    v = metrics.get("live_cmd_markers_total")
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False, 0, f"live_cmd_markers_total missing/non-numeric: {v!r}"
    return v > 0, v, (
        f"live_cmd_markers_total={v!r} -- "
        + ("CMD markers fired live; the #222 orphan-close path (CMD_END) "
           "was actually exercised by this capture" if v > 0 else
           "ZERO CMD markers this capture -- no pid escalated to a live "
           "watchpoint, so the #222 orphan-close fix is UNPROVEN (and "
           "inert) for this run; not a harness failure, a coverage gap"))


def capture_has_events_ok(num_events):
    """Raw floor (issue #157 bypass-suite item): a capture that recorded
    ZERO events is void -- every downstream check (conservation, the
    walk's own per-tab renders) could plausibly report clean-looking
    values against an empty/near-empty trace, which is exactly "the
    product is broken but the checks vacuously pass". num_events comes
    straight off pgwt-server's own `info` response (src/server.c
    handle_info's `num_events`, never inferred). Anything that is not a
    real positive count (missing, None, non-numeric, zero, negative) fails
    -- a gate that cannot see the count must refuse, not assume it is
    fine."""
    ok = isinstance(num_events, (int, float)) and not isinstance(num_events, bool) \
        and num_events > 0
    detail = f"num_events={num_events!r}"
    return ok, detail


# Daemon log lines that mean "this run is broken", scanned literally (not a
# regex over the whole file, to keep a stray user query string that happens
# to contain the word "error" from ever matching -- these are the daemon's
# OWN fprintf(stderr, ...) prefixes, see src/*.c) -- CLAUDE.md: this check
# is never widened or exempted, it is either fixed or reported.
_DAEMON_ERROR_PREFIXES = ("ERROR:", "FATAL:")

# WARN lines that indicate a degraded capture path (backend-status-layout
# auth-free fallback, CPU-accounting legacy gap-inference, escalation
# engine unavailable, etc. -- see src/daemon.c). These refuse a full-mode
# rehearsal even when announced, and stay visible in the report.
_DEGRADED_TIER_MARKERS = (
    "fallback", "fallbacks", "degraded", "LEGACY gap-inference",
    "cannot load", "unavailable",
)


def daemon_log_clean(log_text):
    """Scans a daemon log (tests/ui_live_smoke.sh / tests/demo_rehearsal.sh
    DAEMON_LOG) for the daemon's own ERROR:/FATAL: lines and for WARN lines
    naming a degraded-tier fallback.

    Returns (ok, error_lines, degraded_warnings). Both lists must be empty:
    a fallback/LEGACY tier is announced absence, never a clean full-mode
    rehearsal. Unrelated WARN lines remain informational."""
    error_lines = []
    degraded_warnings = []
    for line in log_text.splitlines():
        stripped = line.strip()
        if stripped.startswith(_DAEMON_ERROR_PREFIXES):
            error_lines.append(stripped)
        elif stripped.startswith("WARN:") and any(
                marker in stripped for marker in _DEGRADED_TIER_MARKERS):
            degraded_warnings.append(stripped)
    return (not error_lines and not degraded_warnings, error_lines, degraded_warnings)


def tab_coverage_check_ok(results, expected_tabs=None):
    """Aggregate tests/demo_workload_coverage.py's per-tab {ok, detail}
    results (issue #214) into ONE gating check: (ok, detail).

    This is the piece that makes #214's own goal real -- "an empty tab
    becomes a test failure, not something the owner notices on stage".
    Without this aggregation wired into demo_rehearsal.py's extra_checks,
    the per-tab checker is a tool someone has to remember to run by hand;
    with it, a tab silently going empty on a future rehearsal fails the
    run's own `ok`, the same way every other extra_checks entry does (this
    module has no separate "informational" registry -- build_demo_summary
    gates unconditionally on every extra_checks entry, so registering it
    there IS registering it as gating; there is no way to add a field that
    is merely recorded).

    expected_tabs, when given, is asserted against results' own key set
    (demo_rehearsal.py always passes demo_workload_coverage.TAB_ORDER) --
    a coverage run that silently checked fewer tabs than it should have
    (a bug in the wiring, not in a checker) must fail loudly here too,
    the same "cannot see -> refuse" rule every checker in this repo
    follows, rather than passing on partial coverage of its own tab list.

    A tab result carrying could_not_evaluate=True (demo_workload_coverage's
    COULD_NOT_EVALUATE_CODES -- pgwt-server refused the query for a
    capacity/tooling reason, e.g. window_too_large on a demo-length
    capture, not because the tab is genuinely empty) is reported in its
    OWN bucket, separate from "not populated" -- review finding (#214
    round 3): conflating the two would send someone chasing an empty tab
    that was never empty. Both buckets still fail this check (fail-closed,
    never a silent pass, never counted as "populated") -- this is a
    naming/legibility distinction in the detail string, not a second way
    to pass."""
    if not isinstance(results, dict) or not results:
        return False, "no per-tab coverage results at all (empty or non-dict)"
    if expected_tabs is not None and set(results.keys()) != set(expected_tabs):
        missing = set(expected_tabs) - set(results.keys())
        extra = set(results.keys()) - set(expected_tabs)
        return False, (f"coverage checked the wrong tab set -- "
                        f"missing={sorted(missing)}, unexpected={sorted(extra)}")
    not_populated = sorted(tab for tab, r in results.items()
                            if not r.get("ok") and not r.get("could_not_evaluate"))
    could_not_evaluate = sorted(tab for tab, r in results.items()
                                 if not r.get("ok") and r.get("could_not_evaluate"))
    ok = len(not_populated) == 0 and len(could_not_evaluate) == 0
    if ok:
        return True, f"all {len(results)} tabs populated"
    parts = []
    if not_populated:
        parts.append(f"{len(not_populated)} tab(s) not populated: {not_populated}")
    if could_not_evaluate:
        parts.append(f"{len(could_not_evaluate)} tab(s) COULD NOT EVALUATE "
                      f"(capacity/tooling refusal, NOT evidence of an empty "
                      f"tab): {could_not_evaluate}")
    return False, "; ".join(parts)


# ── Execution cardinality (#464) ─────────────────────────────────────────
#
# PostgreSQL emits query__execute__start/done from TWO code sites for
# plannable non-SELECT work (around PortalRun, and again inside the static
# helper PortalRunMulti calls); the daemon attached by probe NAME, so every
# DML statement was counted twice. The recorded shape, from
# tests/results/demo-video-48e1bce: Execs 368 against Plans 184 for the DML
# query, and 91 against 91 for the SELECT that never reaches the second
# site. BOTH counted rehearsals passed in that state, because nothing
# anywhere checked execution CARDINALITY -- only durations, conservation and
# cross-tab agreement, none of which change when a count doubles.
#
# This evaluator is the ratio half of the gate: for a complete, successful,
# plannable statement captured with BOTH its planning and its execution
# boundaries, one planning pass corresponds to one execution. The live gate
# (tests/test_execution_cardinality.py) adds the half this one cannot
# provide -- an independently counted N, from the client that issued the
# statements -- because an equality between two numbers the capture itself
# produced cannot catch a defect that scales both.

# Window edges, nothing else. A plan whose execution started before `from`
# (or an execution whose plan ended after `to`) contributes to one count and
# not the other, at most once per edge per query. This is NOT a tolerance
# for runner noise: the defect it has to catch is a FACTOR, and 368 vs 184
# fails a slack of 2 by 182.
EXEC_CARDINALITY_EDGE_SLACK = 2

# First words that make a statement a utility command rather than a
# plannable one. Utility statements go through PortalRun with no
# pg_plan_query pass at all (plan_count 0) and some, like EXECUTE, run a
# cached plan -- the equality does not apply to either, so they are skipped
# EXPLICITLY and reported as skipped, never silently folded into a pass.
_UTILITY_FIRST_WORDS = frozenset("""
    alter analyze begin call checkpoint close cluster comment commit copy
    create deallocate declare discard do drop end execute explain fetch
    grant import listen load lock move notify prepare reassign refresh
    reindex release reset revoke rollback savepoint security set show start
    truncate unlisten vacuum
""".split())


def _as_count(value):
    """A count that is really a count: a non-negative integer. None,
    absent, a string, a float with a fraction or a negative number all mean
    "this row cannot be evaluated", never 0 -- a row whose counts could not
    be parsed must be reported as unevaluable, because treating an
    unparseable field as zero makes exec == plan == 0 and reads as a pass."""
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value if value >= 0 else None
    if isinstance(value, float):
        if value < 0 or value != int(value):
            return None
        return int(value)
    return None


def _first_word(text):
    if not isinstance(text, str):
        return ""
    for token in text.strip().split():
        return token.strip("(").lower()
    return ""


def evaluate_execution_cardinality(rows, expected=None,
                                   edge_slack=EXEC_CARDINALITY_EDGE_SLACK):
    """Did each captured plannable statement execute once per planning pass?

    `rows` is top_queries' row list (each a dict with `text`, `query_id`,
    `exec_count`, `plan_count` exactly as src/server.c serialises them).

    `expected` is the independent cohort count, when the caller has one:
    {substring_of_the_statement_text: N}. Every key MUST match exactly one
    evaluable row and that row's exec_count MUST equal N. This is the half
    that catches uniform doubling -- `exec == plan` alone passes at 2N/2N,
    and `plan <= exec` (the only inequality a prepared-protocol cohort can
    assert) passes at any multiple. A missing key is a FAILURE, not a skip:
    "the statement I issued 24 times produced no row" is the shape a
    refusing capture has, and it must not read as agreement.

    Returns a dict with ok / detail / evaluated / failures / skipped. `ok`
    is False when there is nothing to evaluate: a row list that is empty,
    all-utility or all-unparseable cannot see a doubling, and a gate that
    cannot see must refuse.
    """
    if rows is None:
        return {"ok": False, "detail": "no rows (query refused or errored)",
                "evaluated": 0, "failures": ["rows_absent"], "skipped": []}
    try:
        row_list = list(rows)
    except TypeError:
        return {"ok": False, "detail": f"rows not iterable: {rows!r}",
                "evaluated": 0, "failures": ["rows_unusable"], "skipped": []}

    failures = []
    skipped = []
    evaluated = []

    for row in row_list:
        if not isinstance(row, dict):
            skipped.append(f"non-dict row {row!r}")
            continue
        text = row.get("text") or row.get("query_text") or ""
        label = (text.strip().replace("\n", " ")[:60]
                 or f"query_id={row.get('query_id')}")
        word = _first_word(text)
        if not word:
            skipped.append(f"{label}: no statement text captured")
            continue
        if word in _UTILITY_FIRST_WORDS:
            skipped.append(f"{label}: utility statement ({word})")
            continue
        execs = _as_count(row.get("exec_count"))
        plans = _as_count(row.get("plan_count"))
        if execs is None or plans is None:
            skipped.append(f"{label}: exec_count={row.get('exec_count')!r} "
                           f"plan_count={row.get('plan_count')!r} "
                           f"not usable counts")
            continue
        if plans == 0:
            # Planning boundaries absent: a cached/generic plan, or a window
            # that opened mid-statement. Nothing to compare against.
            skipped.append(f"{label}: no planning pass captured "
                           f"(exec_count={execs})")
            continue
        if execs == 0:
            # ABANDONED PLANS, which the brief's scope excludes explicitly:
            # every occurrence of this statement was planned and then
            # errored or was cancelled before EXEC_DONE, so it has no
            # completed execution to compare the plan against. Measured on
            # gate-1: a deliberately failing `UPDATE ... SET v = v / 0`
            # reports exec_count 0 against plan_count 4.
            #
            # This is safe to skip ONLY because of the refusal below: if the
            # execution probes stopped firing altogether, EVERY plannable
            # row lands here, nothing is left to evaluate, and the verdict
            # is a refusal rather than a pass. A partial loss is caught by
            # the independent-N leg, where a cohort that produced no
            # evaluable row is a failure and not a skip.
            skipped.append(f"{label}: planned {plans}x with no completed "
                           f"execution (abandoned plans / errors)")
            continue
        evaluated.append((label, execs, plans, row))
        if execs > plans + edge_slack:
            failures.append(f"{label}: exec_count {execs} exceeds plan_count "
                            f"{plans} by more than {edge_slack} "
                            f"(ratio {execs / plans:.2f})")
        elif execs + edge_slack < plans:
            failures.append(f"{label}: exec_count {execs} falls short of "
                            f"plan_count {plans} by more than {edge_slack}")

    expected = expected or {}
    for needle, want in expected.items():
        matches = [e for e in evaluated if needle in e[0] or needle in
                   (e[3].get("text") or e[3].get("query_text") or "")]
        if not matches:
            failures.append(f"expected statement {needle!r} (N={want}) "
                            f"produced no evaluable row")
            continue
        if len(matches) > 1:
            failures.append(f"expected statement {needle!r} matched "
                            f"{len(matches)} rows; cohort is not isolated")
            continue
        label, execs, plans, _row = matches[0]
        if execs != want:
            failures.append(f"{label}: exec_count {execs} != independently "
                            f"counted N={want}")

    if not evaluated:
        return {"ok": False,
                "detail": f"nothing evaluable among {len(row_list)} row(s); "
                          f"skipped: {skipped[:6]}",
                "evaluated": 0,
                "failures": failures + ["no_evaluable_rows"],
                "skipped": skipped}

    ok = not failures
    detail = (f"{len(evaluated)} plannable statement(s) evaluated, "
              f"{len(skipped)} skipped"
              if ok else "; ".join(failures[:6]))
    return {"ok": ok, "detail": detail, "evaluated": len(evaluated),
            "failures": failures, "skipped": skipped,
            "rows": [{"label": e[0], "exec_count": e[1], "plan_count": e[2]}
                     for e in evaluated]}


# Every check demo_rehearsal.py MUST have inserted into extra_checks before
# build_demo_summary() may return ok=True.
#
# Why this exists: build_demo_summary iterates extra_checks and fails on any
# entry whose "ok" is false -- which means DELETING a check, or never
# reaching the code that inserts it (an early `continue`, a swallowed
# exception, a renamed key), removed it from the verdict silently. The run
# still printed "DEMO REHEARSAL: PASS". That is how #464's double count
# survived two counted rehearsals in a different way than the missing
# cardinality check did: the gate was not wrong, it was absent, and absence
# was indistinguishable from agreement.
#
# Membership is checked by NAME against this list, so a check that is not
# inserted fails the run exactly as loudly as one that is inserted and
# fails. Adding a mandatory check means adding its name here in the same
# commit.
REQUIRED_EXTRA_CHECKS = (
    "capture_has_events",
    "tab_coverage",
    "time_model_conserves",
    "waterfall_query_latency",
    "execution_cardinality",
    "cross_tab_db_time_agreement",
    "cross_tab_class_attribution",
    "cross_tab_aas_agreement",
    "capture_freshness",
    "daemon_integrity",
    "daemon_log_clean",
)


def build_demo_summary(pass_results, extra_checks, expected_tabs_per_pass=None,
                       required_checks=None):
    """pass_results: list of {"pass": name, "tabs": {tab_id: tab_result}}
    (tab_result is exactly ui_live_smoke_lib.build_tab_result's output).
    extra_checks: dict of check_name -> {"ok": bool, ...}.
    expected_tabs_per_pass: when given (demo_rehearsal.py always passes
    len(ui_live_smoke_lib.TABS) -- criteria doc §2 "all 11 tabs reached"),
    every pass must have reached EXACTLY that many tabs, not merely "more
    than zero".

    Overall `ok` uses the RAW `ok` of every tab result in every pass --
    deliberately ignoring ui_live_smoke_lib's known_failing/xpass
    machinery: issue #157 is explicit that #100/#101 get NO pass here,
    this is the instrument that decides whether a real viewer sees the
    bug, not a gating-CI exemption list. Returns the full summary dict
    written to summary.json.

    Raw floor (bypass-suite item): total_tabs (the sum of tabs actually
    walked across every pass) must be > 0. Without this, a walk where the
    tab loop never ran at all (empty `tabs` dict per pass -- "no tab ever
    loaded") produces zero failed entries by construction (there is
    nothing to iterate and find failing) and would otherwise read as a
    vacuous PASS."""
    failed = []
    total_tabs = 0
    incomplete_passes = []
    # Required-check MEMBERSHIP, before anything else (#464). Iterating
    # extra_checks can only find an entry that is there and failing; it
    # cannot find one that was never inserted, so deleting a mandatory
    # check used to pass silently. `required_checks` defaults to None for
    # the unit tests that drive this function with a one-element dict, but
    # demo_rehearsal.py passes REQUIRED_EXTRA_CHECKS and
    # tests/test_rehearsal_verdict_hardening.py asserts that it does -- an
    # unused parameter is just a differently shaped absence.
    missing_required = []
    if required_checks:
        present = set(extra_checks or {})
        missing_required = [name for name in required_checks
                            if name not in present]
    for p in pass_results:
        n = len(p["tabs"])
        total_tabs += n
        if expected_tabs_per_pass is not None and n != expected_tabs_per_pass:
            incomplete_passes.append(
                f"{p['pass']} ({n}/{expected_tabs_per_pass} tabs reached)")
        for tab_id, result in p["tabs"].items():
            if not result.get("ok"):
                failed.append(f"{p['pass']}/{tab_id}")
    for name, check in extra_checks.items():
        if not check.get("ok"):
            failed.append(name)
    for entry in incomplete_passes:
        failed.append(f"incomplete_pass:{entry}")
    for name in missing_required:
        failed.append(f"missing_required_check:{name}")
    ok = (len(failed) == 0 and len(pass_results) > 0
          and len(extra_checks) > 0 and total_tabs > 0
          and len(incomplete_passes) == 0
          and len(missing_required) == 0)
    return {
        "ok": ok,
        "passes": {p["pass"]: p["tabs"] for p in pass_results},
        "checks": extra_checks,
        "failed": failed,
        "total_tabs_walked": total_tabs,
        # Recorded in summary.json so a reader can tell "every mandatory
        # check ran" from "nobody asked for any".
        "required_checks": list(required_checks or ()),
        "missing_required_checks": missing_required,
    }


def verdict_line(summary):
    """The single verdict line issue #157 requires printed at the end of
    the run."""
    if summary["ok"]:
        return "DEMO REHEARSAL: PASS"
    return f"DEMO REHEARSAL: FAIL (failed: {', '.join(summary['failed'])})"


def resolve_tag(cli_tag, auto_detect_fn):
    """The value for summary.json's "tag" field (docs/DEMO_REHEARSAL_
    CRITERIA.md "a counted attempt names the tag it ran against, ... same
    tag, not merely the same tree"): the --tag CLI value when the caller
    gave a non-empty one, else whatever auto_detect_fn() returns.

    auto_detect_fn is injected (never called directly here) so this stays
    a pure function testable without a real git checkout --
    demo_rehearsal.py passes its own _detect_git_tag (`git describe --tags
    --exact-match` against this checkout, None if it is not exactly on a
    tag or .git is unavailable -- e.g. scripts/demo-rehearsal.sh's rsync
    excludes .git on the remote target this actually runs on).

    An empty string is treated the same as no CLI value (falls through to
    auto_detect_fn) rather than being recorded as a literal "" tag --
    argparse's own default is None, but a caller threading a shell
    variable through (scripts/demo-rehearsal-remote-run.sh's TAG="${5:-}")
    can just as easily produce "" for "not given".

    Returns None (never a commit sha or any other invented substitute) when
    neither the CLI value nor auto_detect_fn() yields one -- an
    unattributed attempt must read as unattributed, not silently uncounted
    under a value that looks like a real tag."""
    return cli_tag or auto_detect_fn()
