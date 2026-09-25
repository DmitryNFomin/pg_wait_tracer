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
# ms-rounding noise, but far tighter than any real missing component would
# produce (a genuinely dropped wait class is a double-digit-percent gap,
# not a rounding error).
TIME_MODEL_TOLERANCE_PCT = 1.0

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
    double-count if included).

    Returns (ok, detail) -- detail always states the actual numbers, never
    just true/false, since a failure here IS the finding."""
    if db_time_ms <= 0:
        return True, "db_time_ms <= 0 (empty/idle window) -- nothing to conserve"
    class_ms = sum(r.get("ms", 0.0) for r in rows if r.get("indent") == 1)
    gap_ms = db_time_ms - class_ms
    gap_pct = abs(gap_ms) / db_time_ms * 100.0
    ok = gap_pct <= tolerance_pct
    detail = (f"db_time_ms={db_time_ms:.1f} class_rows_sum_ms={class_ms:.1f} "
              f"gap={gap_ms:.1f}ms ({gap_pct:.2f}%, tolerance {tolerance_pct}%)")
    return ok, detail


def build_time_model_check(recent_ok, recent_detail, recent_used_raw_path,
                           full_ok, full_detail, full_used_raw_path):
    """Combines demo_rehearsal.py's two time_model queries (see
    RECENT_WINDOW_S's comment above for why there are two). recent_* is
    the short trailing window that forces the raw/exact compute path --
    the ONLY one with real detection power, and the only one that gates
    `ok`. full_* is the whole-capture-window query, kept purely as a
    labeled, non-gating diagnostic (it is answered by the summary path on
    every real run and cannot fail for the bug class this check exists to
    catch).

    recent_used_raw_path/full_used_raw_path: whether demo_rehearsal.py
    actually OBSERVED the raw path in that response (the presence of the
    `categories` key, which only the raw/non-summary handler emits) --
    not merely assumed from the window size. If the recent window did NOT
    get the raw path, the check fails loudly instead of silently trusting
    a result that may be just as vacuous as the whole-window one."""
    ok = bool(recent_ok and recent_used_raw_path)
    return {
        "ok": ok,
        "recent_window": {
            "ok": recent_ok, "detail": recent_detail,
            "compute_path": "raw" if recent_used_raw_path else "summary",
            "window_s": RECENT_WINDOW_S,
        },
        "full_window": {
            "ok": full_ok, "detail": full_detail,
            "compute_path": "raw" if full_used_raw_path else "summary",
            "note": ("informational only -- does not gate `ok`; the "
                     "summary compute path (src/compute.c "
                     "tm_summary_visitor) adds the same value to "
                     "db_time_ns and its class row in the same "
                     "statement, so this assertion holds by construction "
                     "and cannot detect a real conservation bug"),
        },
    }


def waterfall_latency_ok(elapsed_s, threshold_s=WATERFALL_QUERY_THRESHOLD_S):
    return elapsed_s is not None and elapsed_s <= threshold_s


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


def build_demo_summary(pass_results, extra_checks):
    """pass_results: list of {"pass": name, "tabs": {tab_id: tab_result}}
    (tab_result is exactly ui_live_smoke_lib.build_tab_result's output).
    extra_checks: dict of check_name -> {"ok": bool, ...}.

    Overall `ok` uses the RAW `ok` of every tab result in every pass --
    deliberately ignoring ui_live_smoke_lib's known_failing/xpass
    machinery: issue #157 is explicit that #100/#101 get NO pass here,
    this is the instrument that decides whether a real viewer sees the
    bug, not a gating-CI exemption list. Returns the full summary dict
    written to summary.json."""
    failed = []
    for p in pass_results:
        for tab_id, result in p["tabs"].items():
            if not result.get("ok"):
                failed.append(f"{p['pass']}/{tab_id}")
    for name, check in extra_checks.items():
        if not check.get("ok"):
            failed.append(name)
    ok = len(failed) == 0 and len(pass_results) > 0 and len(extra_checks) > 0
    return {
        "ok": ok,
        "passes": {p["pass"]: p["tabs"] for p in pass_results},
        "checks": extra_checks,
        "failed": failed,
    }


def verdict_line(summary):
    """The single verdict line issue #157 requires printed at the end of
    the run."""
    if summary["ok"]:
        return "DEMO REHEARSAL: PASS"
    return f"DEMO REHEARSAL: FAIL (failed: {', '.join(summary['failed'])})"
