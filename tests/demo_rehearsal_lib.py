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
# time_model_over_attribution_ok / time_model_offcpu_cap_ok below for the
# checks that actually have power against that bug class). This identity
# only catches OVER-attribution large enough to drive the residual
# negative past the clamp, or a genuinely corrupted rows array.
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
    just true/false, since a failure here IS the finding."""
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


# Off-CPU* ("CPU (waiting for a core)" -- issue #190's recolour) is ITSELF
# a residual: a dropped wait class's time flows INTO it, not out of the
# identity above, so an anomalously large Off-CPU* is the closest thing to
# a direct under-attribution detector available without new instrumentation
# (owner note, 2026-09-27): on a box where pgbench clients <= physical
# cores, the measured run-queue (waiting-for-a-core) share was 4.92
# percentage points of DB Time on one machine that day. 10% is double
# that -- one machine's worth of headroom, generous enough not to fire on
# ordinary run-queue contention, still far under the double-digit-percent
# size a genuinely dropped wait class would produce (this file's own
# TIME_MODEL_TOLERANCE_PCT comment). This is a FIRST bound, not a
# permanent one -- replace with a comparison against the AAS "CPU (waiting
# for a core)" band integral once that plumbing exists.
OFFCPU_CAP_PCT = 10.0


def time_model_offcpu_cap_ok(offcpu_ms, db_time_ms, has_measured_cpu,
                             cap_pct=OFFCPU_CAP_PCT):
    """Only meaningful when has_measured_cpu (v3 exact-CPU data) -- the
    legacy/sampled tier never computes a real Off-CPU* value (compute.h:
    "there is NO Off-CPU* row... the quantity is unavailable, not zero"),
    so this check is vacuously ok there -- REPORTED as such, not silently
    skipped, so a reader can tell the difference between "checked and
    clean" and "not applicable this tier"."""
    if not has_measured_cpu:
        return True, "has_measured_cpu=False (legacy/sampled tier -- no Off-CPU* signal to check)"
    pct = (offcpu_ms / db_time_ms * 100.0) if db_time_ms > 0 else 0.0
    ok = pct <= cap_pct
    detail = f"offcpu_ms={offcpu_ms:.2f} ({pct:.2f}% of db_time_ms), cap {cap_pct}%"
    return ok, detail


def evaluate_time_model_window(rows, db_time_ms, cpu_clamped_ms,
                               offcpu_ms, has_measured_cpu, used_raw_path,
                               aas=None, check_workload_signature=False,
                               check_aas_floor=False):
    """The full per-window gate for one time_model response: the identity
    conservation check (with its own nonzero-DB-Time floor), the raw/exact
    compute path requirement, the cpu_clamped_ms self-check (CPU-class
    gaps only -- see its own comment), the Off-CPU* cap, and (when
    requested) the criteria-doc #2 floors -- the workload's own signature
    events present with real time, and a non-vacuous AAS reading. ALL
    requested checks must hold, checked in that order (raw floors before
    any ratio/equality, per CLAUDE.md/issue #157's bypass-suite
    requirement). Used both for the repeated in-capture samples (gating --
    see conservation_sample_interval_s, which also requests the two extra
    floors) and for the one-off whole-capture-window diagnostic (never
    gating -- see RECENT_WINDOW_S's comment; the caller leaves
    check_workload_signature/check_aas_floor False there since a 35-45 min
    whole-window AAS average is not "the 60s recent window" the criteria
    doc's floor is stated against).

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
        off_ok, off_detail = time_model_offcpu_cap_ok(
            offcpu_ms, db_time_ms, has_measured_cpu)
    else:
        clamped_ok, clamped_detail = False, (
            "db_time_ms below the nonzero-DB-Time floor -- cpu_clamped_ms "
            "check skipped, not vacuously ok")
        off_ok, off_detail = False, (
            "db_time_ms below the nonzero-DB-Time floor -- Off-CPU* cap "
            "check skipped, not vacuously ok")
    ok = bool(cons_ok and used_raw_path and clamped_ok and off_ok)
    result = {
        "ok": ok,
        "compute_path": "raw" if used_raw_path else "summary",
        "conservation": {"ok": cons_ok, "detail": cons_detail},
        "cpu_clamped": {"ok": clamped_ok, "detail": clamped_detail},
        "offcpu_cap": {"ok": off_ok, "detail": off_detail},
    }
    if check_workload_signature:
        sig_ok, sig_detail = workload_signature_present_ok(rows)
        result["ok"] = bool(result["ok"] and sig_ok)
        result["workload_signature"] = {"ok": sig_ok, "detail": sig_detail}
    if check_aas_floor:
        aas_ok, aas_detail = aas_floor_ok(aas)
        result["ok"] = bool(result["ok"] and aas_ok)
        result["aas_floor"] = {"ok": aas_ok, "detail": aas_detail}
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
    ok = len(samples) > 0 and all(s.get("ok") for s in samples)
    failed_offsets = [round(s.get("offset_s", -1), 1) for s in samples
                      if not s.get("ok")]
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

# tests/live_loop_workload.py's holder/waiter/sleeper loop creates BOTH of
# these events, by construction, on every iteration -- their absence from a
# window means this window captured something other than the intended
# workload (a dead workload process, a misrouted window, a misclassified
# event), not a legitimately quiet window. Full "Class:Event" names, exactly
# as src/wait_event.c pgwt_event_full_name emits them into a time_model
# response's sub-event (indent==2) rows.
REQUIRED_WORKLOAD_EVENTS = ("Lock:relation", "Timeout:PgSleep")


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


# PROVISIONAL (criteria doc §2): its only job is to be non-vacuous -- reject
# an AAS reading of ~0 the same way MIN_DB_TIME_MS rejects a ~0 db_time_ms.
# Replaced by half of the first clean rehearsal's own measured AAS, recorded
# in docs/DEMO_REHEARSAL_CRITERIA.md by a commit that says so, before the
# sequence is claimed -- never silently tightened or loosened here.
AAS_FLOOR_PROVISIONAL = 0.5


def aas_floor_ok(aas, floor=AAS_FLOOR_PROVISIONAL):
    ok = isinstance(aas, (int, float)) and not isinstance(aas, bool) and aas >= floor
    detail = f"aas={aas!r} (floor {floor})"
    return ok, detail


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
# 200ms). This is reporting only (owner instruction, 2026-09-28: "report
# the drift; do not try to fix the scheduling in this branch") -- it never
# gates `ok`. Un-caught coverage loss here would be the NEXT version of
# the same blindness this whole bypass suite exists to close: a gate that
# silently never looks at the interval where the defect lives.
def sweep_offset_drift(tick_record):
    """tick_record: one ui_live_smoke_lib.build_sweep_tick_record() dict
    (target_offsets_ms, achieved_offsets_ms, same length/order -- already
    present in every tab result's blink_sweep.ticks, no new instrumentation
    needed). Returns a dict with per-offset drift_ms (achieved - target)
    and the first (200ms target) offset's own drift, since that is where
    this finding's transients live."""
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
    }


def summarize_sweep_offset_coverage(tab_id, pass_name, blink_sweep_ticks):
    """blink_sweep_ticks: a tab result's blink_sweep.ticks list (one
    build_sweep_tick_record() dict per attempted tick). Returns
    {"tab", "pass", "ticks": [sweep_offset_drift(...) per tick]} -- a
    reporting-only summary, joined at the call site into
    summary.json/print output so a human never has to hand-diff
    target_offsets_ms against achieved_offsets_ms across every tick of
    every tab to notice a coverage gap."""
    return {
        "tab": tab_id,
        "pass": pass_name,
        "ticks": [sweep_offset_drift(t) for t in (blink_sweep_ticks or [])],
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
# engine unavailable, etc. -- see src/daemon.c). These do not fail the
# check by themselves (they ARE logged, so a degrade here is announced, not
# silent -- the issue's actual requirement) but must never be silently
# dropped from the report either.
_DEGRADED_TIER_MARKERS = (
    "fallback", "fallbacks", "degraded", "LEGACY gap-inference",
    "cannot load", "unavailable",
)


def daemon_log_clean(log_text):
    """Scans a daemon log (tests/ui_live_smoke.sh / tests/demo_rehearsal.sh
    DAEMON_LOG) for the daemon's own ERROR:/FATAL: lines and for WARN lines
    naming a degraded-tier fallback.

    Returns (ok, error_lines, degraded_warnings). ok is False iff any
    ERROR:/FATAL: line is present -- a degraded-tier WARN alone does not
    fail the check (it was logged, i.e. announced, which is exactly what
    issue #157 asks for), but is still returned so the summary never hides
    it."""
    error_lines = []
    degraded_warnings = []
    for line in log_text.splitlines():
        stripped = line.strip()
        if stripped.startswith(_DAEMON_ERROR_PREFIXES):
            error_lines.append(stripped)
        elif stripped.startswith("WARN:") and any(
                marker in stripped for marker in _DEGRADED_TIER_MARKERS):
            degraded_warnings.append(stripped)
    return (len(error_lines) == 0, error_lines, degraded_warnings)


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
    follows, rather than passing on partial coverage of its own tab list."""
    if not isinstance(results, dict) or not results:
        return False, "no per-tab coverage results at all (empty or non-dict)"
    if expected_tabs is not None and set(results.keys()) != set(expected_tabs):
        missing = set(expected_tabs) - set(results.keys())
        extra = set(results.keys()) - set(expected_tabs)
        return False, (f"coverage checked the wrong tab set -- "
                        f"missing={sorted(missing)}, unexpected={sorted(extra)}")
    failed = sorted(tab for tab, r in results.items() if not r.get("ok"))
    ok = len(failed) == 0
    if ok:
        return True, f"all {len(results)} tabs populated"
    return False, f"{len(failed)}/{len(results)} tab(s) not populated: {failed}"


def build_demo_summary(pass_results, extra_checks, expected_tabs_per_pass=None):
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
    ok = (len(failed) == 0 and len(pass_results) > 0
          and len(extra_checks) > 0 and total_tabs > 0
          and len(incomplete_passes) == 0)
    return {
        "ok": ok,
        "passes": {p["pass"]: p["tabs"] for p in pass_results},
        "checks": extra_checks,
        "failed": failed,
        "total_tabs_walked": total_tabs,
    }


def verdict_line(summary):
    """The single verdict line issue #157 requires printed at the end of
    the run."""
    if summary["ok"]:
        return "DEMO REHEARSAL: PASS"
    return f"DEMO REHEARSAL: FAIL (failed: {', '.join(summary['failed'])})"
