#!/usr/bin/env python3
"""demo_rehearsal.py -- issue #157: the demo-rehearsal harness.

Walks all eleven tabs REPEATEDLY across a long (default 30-45 min) real
--mode full capture under continuous pgbench + lock/sleep load, instead of
tests/ui_live_smoke.py's single walk near the start of a shorter window.
Deliberately EXTENDS tests/ui_live_smoke.py rather than forking it
(CLAUDE.md): every per-tab check (rendered / clean / no_blink /
color_stability / no_leak) is exactly tests/ui_live_smoke.py's own
`run_tab()`, imported and called once per (pass, tab) -- this module adds
only what issue #157 asks for beyond that:
  - repeating the walk at early/middle/late offsets across the whole
    window (see demo_rehearsal_lib.plan_passes/schedule_passes) instead of
    once near the start;
  - three additional, END-of-capture checks issue #93's live smoke does
    not make: time-model conservation (DB Time == CPU + Waits within
    tolerance), the Waterfall executions query's latency, and the daemon
    log being clean / any degraded-tier fallback being logged, not silent.

Unlike tests/ui_live_smoke.py's KNOWN_FAILING_TABS, NO tab is excused from
the overall verdict here (issue #157: "#100 timeline, #101 waterfall...
get no pass here") -- demo_rehearsal_lib.build_demo_summary uses every raw
tab `ok`, ignoring ui_live_smoke_lib's known_failing/xpass exemption.

Run by tests/demo_rehearsal.sh, which starts the daemon/bridge/pgbench/
workload first, exactly like tests/ui_live_smoke.sh does (issue #157:
"reuse tests/hetzner-vm.sh machinery", "extend, do not fork" -- the two
shell scripts share tests/live_daemon_lib.sh and
tests/live_loop_workload.py for that setup).

Usage:
    python3 tests/demo_rehearsal.py --url http://localhost:8384/ \\
        --trace-dir /tmp/pgwt_demo_XXXXXX --daemon-log /tmp/daemon.log \\
        --duration-min 35 --pgbench-pid 1234 --workload-pid 5678
"""
import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke as live_smoke
import ui_live_smoke_lib as lib
import demo_rehearsal_lib as drlib
import demo_workload_coverage as cov
from server_harness import ServerHarness

from playwright.sync_api import sync_playwright

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "results", "demo_rehearsal")

# The live Waterfall tab's own default window (web/static/lib/state.js
# TimeRange.liveRangeSecs) -- the end-of-capture executions-query timing
# check queries this same trailing window, not the whole capture, to match
# what a viewer's browser is actually asking for at that moment.
WATERFALL_LIVE_WINDOW_S = 900


def _print_plan_banner(duration_s, ticks, passes):
    degraded = ticks < lib.MIN_TICKS or len(passes) < 3
    names = ", ".join(f"{n}@{o:.0f}s" for n, o in passes)
    print(f"demo_rehearsal: plan duration={duration_s:.0f}s "
          f"ticks_per_tab={ticks} passes=[{names}]")
    if degraded:
        print("demo_rehearsal: NOTE this window is too short for the real "
              f"baseline plan (MIN_TICKS={lib.MIN_TICKS}, 3 passes) -- "
              "ticks_per_tab and/or the pass count were reduced. This is "
              "expected and CORRECT for a self-test run (DURATION_MIN=3); "
              "it is never expected for a real baseline (default "
              "DURATION_MIN=35) and some tabs' ticks_ok WILL legitimately "
              "fail below -- that proves the harness's mechanics, not the "
              "product.")


def _assert_daemon_alive(daemon_pid, where):
    """Raw floor (docs/DEMO_REHEARSAL_CRITERIA.md §2: 'the daemon process
    alive for the whole window'). Mirrors live_smoke._assert_workload_alive
    exactly (same module already reuses that private helper directly, see
    the calls below) -- a daemon that died partway through would leave
    every remaining tab/sample grading a frozen or absent trace file, which
    could still read as a clean (if stale) result. None means the caller
    did not wire this up (e.g. a manual --url run)."""
    if daemon_pid is None:
        return
    if not live_smoke._pid_is_live(daemon_pid):
        print(f"FATAL: daemon (pid {daemon_pid}) is no longer running ({where}) -- "
              f"refusing to grade the remaining checks against a dead capture",
              file=sys.stderr)
        sys.exit(1)


def _run_passes(browser, url, out_dir, ticks, passes, first_data_timeout,
                pgbench_pid, workload_pid, daemon_pid, t0):
    pass_results = []
    sweep_coverage = []
    for name, target_offset_s in passes:
        remaining = target_offset_s - (time.monotonic() - t0)
        if remaining > 0:
            print(f"demo_rehearsal: sleeping {remaining:.0f}s to reach the "
                  f"'{name}' pass's target offset ({target_offset_s:.0f}s)")
            time.sleep(remaining)
        # Reviewer finding (round 1): _assert_workload_alive used to fire
        # only after each TAB completed, not right after this multi-minute
        # sleep -- the first tab of a pass could be graded against a
        # workload that had already died during the sleep, minutes
        # earlier. Check immediately on waking, before walking anything.
        live_smoke._assert_workload_alive(
            pgbench_pid, workload_pid,
            f"after sleeping to pass {name!r}'s target offset")
        _assert_daemon_alive(daemon_pid,
                             f"after sleeping to pass {name!r}'s target offset")
        actual_offset = time.monotonic() - t0
        print(f"\n=== pass {name} (target {target_offset_s:.0f}s, "
              f"actual {actual_offset:.0f}s) ===")
        pass_out_dir = os.path.join(out_dir, name)
        tab_results = {}
        for tab_id in lib.TABS:
            print(f"  --- {tab_id} ---")
            result = live_smoke.run_tab(browser, tab_id, url, pass_out_dir,
                                        ticks, first_data_timeout,
                                        lib.BLINK_THRESHOLD)
            tab_results[tab_id] = result
            live_smoke._assert_workload_alive(
                pgbench_pid, workload_pid, f"pass {name!r} tab {tab_id!r}")
            _assert_daemon_alive(daemon_pid, f"pass {name!r} tab {tab_id!r}")
            kf_line = lib.known_failing_report_line(tab_id, result["ok"])
            # A KNOWN-FAILING line would be misleading here -- this harness
            # grants no such exemption (issue #157) -- so a listed tab's
            # raw failure prints plain FAIL, with a note pointing at why
            # ui_live_smoke.py excuses it there but this script does not.
            if kf_line and not result["ok"]:
                status = ("FAIL (ui_live_smoke.py's KNOWN_FAILING_TABS "
                          "excuses this there -- demo_rehearsal grants no "
                          "such exemption)")
            elif kf_line:
                status = f"PASS ({kf_line})"
            else:
                status = "PASS" if result["ok"] else "FAIL"
            print(f"    {status} [{name}/{tab_id}] "
                  f"ticks={result['ticks_observed']} "
                  f"rendered={result['rendered']}")
            # Sweep-offset coverage (owner finding, 2026-09-28): report,
            # never gate, whether the blink-sweep's samples actually landed
            # near their targets -- a late mount can silently push the
            # WHOLE sweep past the early window where a real transient
            # lives (see summarize_sweep_offset_coverage's own comment).
            coverage = drlib.summarize_sweep_offset_coverage(
                tab_id, name, result.get("blink_sweep", {}).get("ticks"))
            sweep_coverage.append(coverage)
            for tick_i, tick_drift in enumerate(coverage["ticks"], start=1):
                fd = tick_drift["first_drift_ms"]
                if fd is not None and fd > 0:
                    target1 = tick_drift["target_offsets_ms"][0]
                    achieved1 = tick_drift["first_achieved_ms"]
                    target2 = (tick_drift["target_offsets_ms"][1]
                              if len(tick_drift["target_offsets_ms"]) > 1 else None)
                    note = ""
                    if target2 is not None and achieved1 is not None and achieved1 >= target2:
                        note = (" -- EARLY WINDOW LIKELY MISSED (first sample "
                               f"landed at/past the {target2}ms target)")
                    print(f"    sweep coverage [{name}/{tab_id}] tick {tick_i}: "
                          f"first offset target={target1}ms achieved={achieved1}ms "
                          f"drift=+{fd}ms{note}")
        pass_results.append({"pass": name, "tabs": tab_results})
    return pass_results, sweep_coverage


def _error_or_none(resp, label):
    """None if resp carries no pgwt-server-side error field; else a detail
    string. Reviewer finding (round 1): a reject_overload/invalid-request
    error response (`{"error":..., "code":..., "hint":...}`) has no
    `rows`/`db_time_ms`/`total_count` at all -- reading those with a
    silent `.get(..., default)` made a genuine server-side refusal look
    exactly like an empty/idle window or a suspiciously-fast empty query,
    both of which read as PASS. Callers must check this BEFORE trusting
    any other field in resp."""
    if isinstance(resp, dict) and "error" in resp:
        return (f"{label} returned an error: {resp.get('error')} "
                f"(code={resp.get('code')}, hint={resp.get('hint')})")
    return None


def _query_time_model_window(srv, from_ns, to_ns, label, gate_on_raw_path,
                             check_criteria_floors=False):
    """One time_model query, evaluated through
    demo_rehearsal_lib.evaluate_time_model_window (identity conservation +
    raw-path check [only when gate_on_raw_path] + the cpu_clamped_ms
    self-check + the Off-CPU* cap + [when check_criteria_floors] the
    criteria-doc #2 workload-signature and AAS floors). Returns that dict,
    or an ok=False stand-in on a server-side error / a malformed response --
    a gate that cannot see the fields it needs must refuse, never assume
    they were fine."""
    tm = srv.query("time_model", from_=from_ns, to_=to_ns,
                   timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
    err = _error_or_none(tm, label)
    if err is not None:
        return {"ok": False, "detail": err}
    rows = tm.get("rows")
    db_time_ms = tm.get("db_time_ms")
    if rows is None or db_time_ms is None:
        return {"ok": False, "detail": f"{label}: response missing rows/db_time_ms: {tm}"}
    used_raw_path = "categories" in tm
    result = drlib.evaluate_time_model_window(
        rows, db_time_ms,
        cpu_clamped_ms=tm.get("cpu_clamped_ms", 0.0),
        offcpu_ms=tm.get("offcpu_ms", 0.0),
        has_measured_cpu=tm.get("has_measured_cpu", False),
        used_raw_path=used_raw_path if gate_on_raw_path else True,
        aas=tm.get("aas"),
        check_workload_signature=check_criteria_floors,
        check_aas_floor=check_criteria_floors)
    result["fidelity"] = tm.get("fidelity")
    result["db_time_ms"] = db_time_ms
    result["observed_raw_path"] = used_raw_path
    return result


def _sample_recent_window(srv, offset_s):
    """One in-capture conservation sample: query the RECENT_WINDOW_S
    trailing window (the only one that forces the raw/exact compute path,
    see demo_rehearsal_lib.RECENT_WINDOW_S) against pgwt-server's CURRENT
    `to_ns`, gated on the raw path actually being observed AND (criteria
    doc §2: this IS "the 60s recent window" the AAS/workload-signature
    floors are stated against) the two extra floors."""
    try:
        info = srv.query("info", timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
    except Exception as e:
        return {"ok": False, "offset_s": offset_s, "detail": f"info query failed: {e!r}"}
    from_ns = int(info.get("from_ns", 0))
    to_ns = int(info.get("to_ns", 0))
    recent_from_ns = max(from_ns, to_ns - int(drlib.RECENT_WINDOW_S * 1_000_000_000))
    try:
        result = _query_time_model_window(
            srv, recent_from_ns, to_ns, "time_model (in-capture sample)",
            gate_on_raw_path=True, check_criteria_floors=True)
    except Exception as e:
        return {"ok": False, "offset_s": offset_s, "detail": f"time_model query failed: {e!r}"}
    result["offset_s"] = offset_s
    return result


def _conservation_sampler_loop(trace_dir, interval_s, stop_event, samples, t0):
    """Runs in a background thread for the whole duration of the walk,
    sampling the recent-window conservation gate every interval_s (owner
    finding, 2026-09-27: 'the audience watches the whole run, not the last
    minute of it' -- a single end-of-capture sample is n=1). A fresh
    ServerHarness (and pgwt-server subprocess) per sample, never a
    connection held open across iterations -- a timed-out query's stale
    response sitting unread on a shared pipe would desync every later
    query on that same connection. Any exception (including a pgwt-server
    hang past its own query timeout) is recorded as a FAILING sample,
    never silently dropped -- a crash here must shrink `ok`, not just the
    sample count."""
    while not stop_event.wait(interval_s):
        offset_s = time.monotonic() - t0
        try:
            with ServerHarness(trace_dir) as srv:
                result = _sample_recent_window(srv, offset_s)
        except Exception as e:
            result = {"ok": False, "offset_s": offset_s,
                      "detail": f"sampler iteration crashed: {e!r}"}
        samples.append(result)


def _waterfall_latency_check(srv, from_ns, to_ns):
    win_from = max(from_ns, to_ns - WATERFALL_LIVE_WINDOW_S * 1_000_000_000)
    start = time.monotonic()
    resp = srv.query("executions", from_=win_from, to_=to_ns, limit=100,
                     timeout=drlib.EXECUTIONS_QUERY_TIMEOUT_S)
    elapsed = time.monotonic() - start
    err = _error_or_none(resp, "executions")
    if err is not None:
        print(f"demo_rehearsal: waterfall_query_latency: FAIL -- {err} "
              f"(answered after {elapsed:.2f}s)")
        return {"ok": False, "elapsed_s": elapsed,
                "threshold_s": drlib.WATERFALL_QUERY_THRESHOLD_S,
                "window_s": WATERFALL_LIVE_WINDOW_S, "error": err}
    ok = drlib.waterfall_latency_ok(elapsed)
    print(f"demo_rehearsal: waterfall_query_latency: {'PASS' if ok else 'FAIL'} "
          f"-- {elapsed:.2f}s (threshold {drlib.WATERFALL_QUERY_THRESHOLD_S}s), "
          f"window={WATERFALL_LIVE_WINDOW_S}s, "
          f"rows={len(resp.get('rows', []))}, "
          f"total_count={resp.get('total_count')}, "
          f"unavailable={resp.get('unavailable')}")
    return {"ok": ok, "elapsed_s": elapsed,
            "threshold_s": drlib.WATERFALL_QUERY_THRESHOLD_S,
            "window_s": WATERFALL_LIVE_WINDOW_S,
            "rows_returned": len(resp.get("rows", [])),
            "total_count": resp.get("total_count"),
            "unavailable": resp.get("unavailable")}


def _daemon_log_check(daemon_log_path):
    try:
        with open(daemon_log_path, errors="replace") as f:
            log_text = f.read()
    except OSError as e:
        print(f"demo_rehearsal: daemon_log_clean: FAIL -- could not read "
              f"{daemon_log_path}: {e}")
        return {"ok": False, "error_lines": [f"could not read log: {e}"],
                "degraded_warnings": []}
    ok, error_lines, degraded_warnings = drlib.daemon_log_clean(log_text)
    print(f"demo_rehearsal: daemon_log_clean: {'PASS' if ok else 'FAIL'} "
          f"-- {len(error_lines)} error line(s), "
          f"{len(degraded_warnings)} degraded-tier warning(s)")
    for line in error_lines:
        print(f"    ERROR LINE: {line}")
    for line in degraded_warnings:
        print(f"    degraded-tier warning (announced, not failing): {line}")
    return {"ok": ok, "error_lines": error_lines,
            "degraded_warnings": degraded_warnings}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True, help="the Go bridge's page URL")
    ap.add_argument("--trace-dir", required=True,
                    help="the daemon's trace dir (direct pgwt-server queries)")
    ap.add_argument("--daemon-log", required=True,
                    help="path to the daemon's stderr log file")
    ap.add_argument("--duration-min", type=float, default=35.0,
                    help="total capture window in minutes (default 35; "
                    "the issue's documented default is 30-45 -- never "
                    "lower this for a real baseline run, only for the "
                    "explicit DURATION_MIN=3 self-test)")
    ap.add_argument("--out-dir", default=RESULTS_DIR)
    ap.add_argument("--first-data-timeout", type=float,
                    default=lib.FIRST_DATA_TIMEOUT_S)
    ap.add_argument("--pgbench-pid", type=int, default=None)
    ap.add_argument("--workload-pid", type=int, default=None)
    ap.add_argument("--daemon-pid", type=int, default=None,
                    help="the pg_wait_tracer daemon's own PID (criteria doc "
                    "#157/§2 'the daemon process alive for the whole window')")
    args = ap.parse_args()

    duration_s = args.duration_min * 60.0
    ticks, passes = drlib.plan_passes(duration_s)
    _print_plan_banner(duration_s, ticks, passes)

    out_dir = args.out_dir
    lib.reset_output_dir(out_dir)
    run_marker = os.environ.get("PGWT_RUN_MARKER", str(int(time.time())))
    with open(os.path.join(out_dir, "run.id"), "w") as f:
        f.write(run_marker)

    live_smoke._assert_workload_alive(args.pgbench_pid, args.workload_pid,
                                      "before the walk")
    _assert_daemon_alive(args.daemon_pid, "before the walk")

    # Sample the conservation gate every few minutes THROUGHOUT the
    # capture, not just once at the end (owner finding, 2026-09-27: "the
    # audience watches the whole run, not the last minute of it"). Runs in
    # a background thread against its own pgwt-server subprocess -- the
    # walk itself never talks to pgwt-server directly (only to the Go
    # bridge via Playwright/HTTP), so the two never contend for anything.
    sample_interval_s = drlib.conservation_sample_interval_s(duration_s)
    conservation_samples = []
    sampler_stop = threading.Event()

    t0 = time.monotonic()
    sampler_thread = threading.Thread(
        target=_conservation_sampler_loop,
        args=(args.trace_dir, sample_interval_s, sampler_stop,
              conservation_samples, t0),
        daemon=True)
    sampler_thread.start()
    print(f"demo_rehearsal: conservation sampler started "
          f"(every {sample_interval_s:.0f}s through the capture)")

    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            pass_results, sweep_coverage = _run_passes(
                browser, args.url, out_dir, ticks, passes,
                args.first_data_timeout, args.pgbench_pid,
                args.workload_pid, args.daemon_pid, t0)
        finally:
            browser.close()

    live_smoke._assert_workload_alive(args.pgbench_pid, args.workload_pid,
                                      "after the walk, before end-of-capture checks")
    _assert_daemon_alive(args.daemon_pid, "after the walk, before end-of-capture checks")

    # Bounded stop: never let a stuck sampler iteration hang the rehearsal
    # itself. The thread is a daemon (won't block process exit either) --
    # this join is best-effort so the LAST in-flight sample, if any, still
    # lands in conservation_samples before we read it.
    sampler_stop.set()
    sampler_thread.join(timeout=min(60.0, sample_interval_s))
    if sampler_thread.is_alive():
        print("demo_rehearsal: WARNING conservation sampler thread did not "
              "stop within its join timeout -- proceeding with whatever "
              "samples it collected so far")

    print("\n=== end-of-capture checks ===")
    extra_checks = {}
    with ServerHarness(args.trace_dir) as srv:
        info = srv.query("info")
        from_ns = int(info["from_ns"])
        to_ns = int(info["to_ns"])

        events_ok, events_detail = drlib.capture_has_events_ok(info.get("num_events"))
        print(f"demo_rehearsal: capture_has_events: "
              f"{'PASS' if events_ok else 'FAIL'} -- {events_detail}")
        extra_checks["capture_has_events"] = {"ok": events_ok, "detail": events_detail}

        # issue #214 review: the per-tab "does this tab have real content"
        # checker MUST run here, before ServerHarness/the trace dir go away
        # -- this `with` block is still inside main(), well before
        # tests/demo_rehearsal.sh's cleanup() trap (rm -rf "$TRACE_DIR")
        # fires on script exit, and before tests/ui_live_smoke.sh's own
        # teardown for the shorter live-smoke walk. Queried over the WHOLE
        # capture window (not RECENT_WINDOW_S) so a tab's content earlier
        # in a long rehearsal still counts. Registered in extra_checks,
        # which build_demo_summary gates on UNCONDITIONALLY (this module
        # has no separate informational-only registry -- everything written
        # here fails the run's own `ok` if not ok, by construction).
        coverage_results = cov.run_coverage(srv, from_ns, to_ns,
                                            num_cpus=info.get("num_cpus"))
        coverage_ok, coverage_detail = drlib.tab_coverage_check_ok(
            coverage_results, expected_tabs=cov.TAB_ORDER)
        print(f"demo_rehearsal: tab_coverage: "
              f"{'PASS' if coverage_ok else 'FAIL'} -- {coverage_detail}")
        for tab in cov.TAB_ORDER:
            r = coverage_results.get(tab, {})
            print(f"    {tab}: {'PASS' if r.get('ok') else 'FAIL'} -- {r.get('detail')}")
        extra_checks["tab_coverage"] = {
            "ok": coverage_ok, "detail": coverage_detail, "tabs": coverage_results,
        }
        # issue #214 review item 3: the per-tab numbers (row counts, band
        # counts, spread ratios, ...) must be auditable from a file, not
        # only from a commit message or a printed line -- written here
        # too, alongside summary.json (which also carries this same dict
        # via extra_checks, but a reviewer should not have to parse the
        # whole rehearsal summary to find it).
        with open(os.path.join(out_dir, "tab_coverage.json"), "w") as f:
            json.dump({
                "from_ns": from_ns, "to_ns": to_ns,
                "num_cpus": info.get("num_cpus"),
                "ok": coverage_ok, "detail": coverage_detail,
                "results": coverage_results,
            }, f, indent=2, sort_keys=True)
            f.write("\n")

        # One final sample right at the true end of the capture -- the
        # sampler thread's own last periodic sample can land up to
        # sample_interval_s early, so this always covers the tail.
        final_sample = _sample_recent_window(srv, time.monotonic() - t0)
        conservation_samples.append(final_sample)

        full_window_result = _query_time_model_window(
            srv, from_ns, to_ns, "time_model (full window)",
            gate_on_raw_path=False)
        conservation_check = drlib.build_demo_conservation_check(
            conservation_samples, full_window_result)
        print(f"demo_rehearsal: time_model_conserves: "
              f"{'PASS' if conservation_check['ok'] else 'FAIL'} "
              f"({conservation_check['num_samples']} in-capture samples, "
              f"failed offsets: {conservation_check['failed_offsets_s']})")
        for s in conservation_samples:
            print(f"    sample @{s.get('offset_s', -1):.0f}s: "
                  f"{'PASS' if s.get('ok') else 'FAIL'} {s.get('detail', s)}")
        print(f"    full window (informational only): "
              f"{'PASS' if full_window_result.get('ok') else 'FAIL'} "
              f"{full_window_result}")
        extra_checks["time_model_conserves"] = conservation_check

        extra_checks["waterfall_query_latency"] = _waterfall_latency_check(srv, from_ns, to_ns)

        # Cross-tab agreement (criteria doc §5): top_events answers the
        # Top Events tab and carries its OWN top-level db_time_ms (not a
        # sum of its own possibly-truncated row list -- src/server.c
        # handle_top_events emits it independently), so this is a safe,
        # direct comparison against time_model's (the Overview tab) number
        # for the IDENTICAL window -- no bucket-weighted re-derivation
        # needed.
        recent_from_ns = max(from_ns, to_ns - int(drlib.RECENT_WINDOW_S * 1_000_000_000))
        top_events_resp = srv.query("top_events", from_=recent_from_ns, to_=to_ns,
                                    timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
        top_events_err = _error_or_none(top_events_resp, "top_events (cross-tab)")
        time_model_recent = srv.query("time_model", from_=recent_from_ns, to_=to_ns,
                                      timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
        time_model_err = _error_or_none(time_model_recent, "time_model (cross-tab)")
        if top_events_err is not None or time_model_err is not None:
            cross_tab_ok = False
            cross_tab_detail = f"query error(s): {top_events_err!r} / {time_model_err!r}"
        else:
            cross_tab_ok, cross_tab_detail = drlib.cross_tab_db_time_agreement_ok(
                time_model_recent.get("db_time_ms"), top_events_resp.get("db_time_ms"))
        print(f"demo_rehearsal: cross_tab_db_time_agreement: "
              f"{'PASS' if cross_tab_ok else 'FAIL'} -- {cross_tab_detail}")
        extra_checks["cross_tab_db_time_agreement"] = {
            "ok": cross_tab_ok, "detail": cross_tab_detail,
            "note": ("compares Overview's time_model against Top Events' "
                    "top_events for the identical recent window; the AAS "
                    "leg (vs. the aas-bucketed Timeline endpoint) is NOT "
                    "implemented -- deriving a comparable aggregate AAS "
                    "from that endpoint's per-bucket per-class values "
                    "needs bucket-weighted summation this branch did not "
                    "implement with confidence in scope; see the PR "
                    "report's gap table"),
        }

        # Freshness (criteria doc §5): the daemon's own latest event vs the
        # server's own wall clock, both from the same `info` response.
        fresh_ok, fresh_detail = drlib.freshness_ok(info.get("now_ns"), to_ns)
        print(f"demo_rehearsal: capture_freshness: "
              f"{'PASS' if fresh_ok else 'FAIL'} -- {fresh_detail}")
        extra_checks["capture_freshness"] = {"ok": fresh_ok, "detail": fresh_detail}

        # Daemon integrity (criteria doc §6): the same control-socket
        # "metrics" command the UI already calls (web/static/lib/control.js
        # controlMetrics()), proxied through pgwt-server -- no new src/
        # instrumentation needed. Queried while the daemon is still up
        # (teardown happens in tests/demo_rehearsal.sh's cleanup(), after
        # this script returns).
        metrics_resp = srv.query("control", request={"cmd": "metrics"},
                                 timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
        metrics_err = _error_or_none(metrics_resp, "control metrics")
        if metrics_err is not None:
            integrity_ok, integrity_detail = False, metrics_err
        else:
            integrity_ok, integrity_detail = drlib.daemon_integrity_ok(
                metrics_resp.get("response"))
        print(f"demo_rehearsal: daemon_integrity: "
              f"{'PASS' if integrity_ok else 'FAIL'} -- {integrity_detail}")
        extra_checks["daemon_integrity"] = {
            "ok": integrity_ok, "detail": integrity_detail,
            "note": ("ringbuf_drops_total/state_map_full_total/"
                    "seen_query_ids_full_total == 0 -- a lost LIFECYCLE "
                    "event is silent (no counter increments), so this "
                    "means no TRACE events were dropped, never that "
                    "nothing was missed; see docs/DEMO_REHEARSAL_CRITERIA.md "
                    "section 6"),
        }

    extra_checks["daemon_log_clean"] = _daemon_log_check(args.daemon_log)

    summary = drlib.build_demo_summary(pass_results, extra_checks,
                                       expected_tabs_per_pass=len(lib.TABS))
    summary["duration_min"] = args.duration_min
    summary["ticks_per_tab"] = ticks
    summary["plan_degraded"] = ticks < lib.MIN_TICKS or len(passes) < 3
    # Reporting only (owner finding, 2026-09-28) -- never gates `ok`. See
    # summarize_sweep_offset_coverage's own comment.
    summary["sweep_offset_coverage"] = sweep_coverage

    summary_path = os.path.join(out_dir, "summary.json")
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")

    print()
    print("════════════════════════════════════════")
    print("  DEMO REHEARSAL SUMMARY")
    print("════════════════════════════════════════")
    for p in pass_results:
        for tab_id, result in p["tabs"].items():
            status = "PASS" if result["ok"] else "FAIL"
            print(f"  {status} {p['pass']}/{tab_id}")
    for name, check in extra_checks.items():
        print(f"  {'PASS' if check['ok'] else 'FAIL'} {name}")
    print(f"  summary: {summary_path}")
    print(drlib.verdict_line(summary))

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
