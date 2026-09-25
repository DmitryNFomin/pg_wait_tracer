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
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke as live_smoke
import ui_live_smoke_lib as lib
import demo_rehearsal_lib as drlib
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


def _run_passes(browser, url, out_dir, ticks, passes, first_data_timeout,
                pgbench_pid, workload_pid, t0):
    pass_results = []
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
        pass_results.append({"pass": name, "tabs": tab_results})
    return pass_results


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


def _query_time_model(srv, from_ns, to_ns, label):
    """One time_model query + its conservation verdict. Returns
    (ok, detail, used_raw_path, fidelity) -- used_raw_path is OBSERVED
    from the response (the `categories` key is only ever emitted by the
    raw/non-summary handler, src/server.c handle_time_model), never
    assumed from the requested window size alone (see
    demo_rehearsal_lib.RECENT_WINDOW_S's comment)."""
    tm = srv.query("time_model", from_=from_ns, to_=to_ns,
                   timeout=drlib.TIME_MODEL_QUERY_TIMEOUT_S)
    err = _error_or_none(tm, label)
    if err is not None:
        return False, err, False, None
    rows = tm.get("rows")
    db_time_ms = tm.get("db_time_ms")
    if rows is None or db_time_ms is None:
        return (False, f"{label}: response missing rows/db_time_ms: {tm}",
                False, tm.get("fidelity"))
    ok, detail = drlib.time_model_conservation(rows, db_time_ms)
    return ok, detail, "categories" in tm, tm.get("fidelity")


def _time_model_check(srv, from_ns, to_ns):
    full_ok, full_detail, full_raw, full_fid = _query_time_model(
        srv, from_ns, to_ns, "time_model (full window)")
    recent_from_ns = max(from_ns,
                         to_ns - int(drlib.RECENT_WINDOW_S * 1_000_000_000))
    recent_ok, recent_detail, recent_raw, recent_fid = _query_time_model(
        srv, recent_from_ns, to_ns, "time_model (recent window)")
    result = drlib.build_time_model_check(
        recent_ok, recent_detail, recent_raw, full_ok, full_detail, full_raw)
    print(f"demo_rehearsal: time_model_conserves: "
          f"{'PASS' if result['ok'] else 'FAIL'}")
    print(f"    recent ({drlib.RECENT_WINDOW_S:.0f}s, "
          f"path={'raw' if recent_raw else 'summary'}, "
          f"fidelity={recent_fid}) [GATES ok]: {recent_detail}")
    print(f"    full window (path={'raw' if full_raw else 'summary'}, "
          f"fidelity={full_fid}, informational only): {full_detail}")
    return result


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

    t0 = time.monotonic()
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            pass_results = _run_passes(
                browser, args.url, out_dir, ticks, passes,
                args.first_data_timeout, args.pgbench_pid,
                args.workload_pid, t0)
        finally:
            browser.close()

    live_smoke._assert_workload_alive(args.pgbench_pid, args.workload_pid,
                                      "after the walk, before end-of-capture checks")

    print("\n=== end-of-capture checks ===")
    extra_checks = {}
    with ServerHarness(args.trace_dir) as srv:
        info = srv.query("info")
        from_ns = int(info["from_ns"])
        to_ns = int(info["to_ns"])
        extra_checks["time_model_conserves"] = _time_model_check(srv, from_ns, to_ns)
        extra_checks["waterfall_query_latency"] = _waterfall_latency_check(srv, from_ns, to_ns)
    extra_checks["daemon_log_clean"] = _daemon_log_check(args.daemon_log)

    summary = drlib.build_demo_summary(pass_results, extra_checks)
    summary["duration_min"] = args.duration_min
    summary["ticks_per_tab"] = ticks
    summary["plan_degraded"] = ticks < lib.MIN_TICKS or len(passes) < 3

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
