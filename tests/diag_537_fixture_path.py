#!/usr/bin/env python3
"""diag_537_fixture_path.py -- issue #537 DIAGNOSTIC, part 2. NOT A GATE.

tests/diag_537_waterfall.py measures the two navigation paths #537 names.
This measures the THIRD path, the one #514's gates actually walked: navigate
(either way), then install the dense-executions fixture, then wait for
`#waterfall-chart canvas` again.

The fixture (verbatim from origin/agent/514-rehearsal-coverage,
tests/web_unit/execution_list_browser.py::install_dense_execution_fixture)
pauses live, wraps transport.request, clicks #executions-sort-toggle, and
answers the resulting `executions` request with 100 clones of
`response.rows[0]`, each clone's start_ns offset by i nanoseconds.

Two things about that are invisible against tests/mock_server.py:

  1. the mock keys execution_detail on filters.pid ONLY and ignores start_ns
     (tests/mock_server.py, `if cmd == "execution_detail"`), so a clone whose
     start_ns names an execution that does not exist still gets a full,
     drawable detail. A real pgwt-server answers the window it was asked for;
  2. the toggle flips sortMode from the shipped default duration_desc
     ("longest running first") to start_desc ("latest first"), so
     response.rows[0] becomes the NEWEST execution -- issue #101's undrawable
     one. Cloning it 100 times makes EVERY row undrawable, which is exactly
     the input pickDefaultExecution() was added to escape, and it cannot:
     rows.find() has nothing drawable to find and falls through to rows[0].

If that is the mechanism, views/waterfall.js's renderDetail() takes its
`!model.hasData` branch, calls disposeChart() and replaces the host's content
with "No execution events captured" -- REMOVING the canvas that was there a
moment earlier. Every later wait_for_selector('#waterfall-chart canvas') then
cannot pass, however long it waits.

So this script records, per attempt: the canvas BEFORE the fixture, the
canvas AFTER it, the sort label each time, the fields of the row that was
cloned, and the execution_detail round trip's own leader-event count. A
"before" canvas that is present and an "after" canvas that is gone is the
mechanism, observed rather than argued.

Fail-safe on the same terms as part 1 (diag_537_waterfall.classify_attempt):
an attempt that never got a canvas BEFORE the fixture proves nothing about
the fixture and is VOID, never a failure.

Usage: tests/diag_537_fixture_path.py --url URL [--attempts 10] [--out FILE]
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diag_537_waterfall import (          # noqa: E402
    CANVAS_TIMEOUT_MS, CONNECT_TIMEOUT_MS, DEVICE_SCALE_FACTOR, VIEWPORT,
    DiagRefusal, _SNAPSHOT_JS, _WS_PROBE_JS, bridge_alive,
)

# Verbatim from origin/agent/514-rehearsal-coverage's
# install_dense_execution_fixture -- the clone expression is the thing under
# test, so it is copied, never paraphrased.
_FIXTURE_JS = r"""
() => {
    const transport = window.__pgwt.transport;
    const original = transport.request.bind(transport);
    window.__executionFixture = null;
    window.__executionFixtureError = null;
    window.__clonedRow = null;
    transport.request = async (channel, cmd, params) => {
        const response = await original(channel, cmd, params);
        if (cmd !== 'executions') return response;
        const first = response.rows[0];
        if (!first) {
            window.__executionFixtureError = 'dense fixture requires a nonempty executions response';
            return response;
        }
        window.__clonedRow = first;
        const rows = Array.from({ length: 100 }, (_, i) => ({ ...first,
            start_ns: String(BigInt(first.start_ns) + BigInt(i)) }));
        const fixture = { ...response, rows, total_count: 265756, truncated: true };
        window.__executionFixture = fixture;
        return fixture;
    };
}
"""

_SORT_LABEL_JS = ("() => (document.getElementById('executions-sort-label') || {})"
                  ".textContent || null")


def classify_fixture_attempt(rec):
    """'canvas_lost' | 'survived' | 'void'.

    void when the attempt could not observe the fixture's effect at all: no
    canvas before it, no fixture installed, or a dead bridge. A sweep of voids
    must never read as "the fixture is harmless".
    """
    if not isinstance(rec, dict):
        raise DiagRefusal("attempt record is not a dict: %r" % (rec,))
    if rec.get("harness_error"):
        return "void"
    if not rec.get("connected"):
        return "void"
    if not rec.get("bridge_alive_after"):
        return "void"
    before = rec.get("canvas_before")
    after = rec.get("canvas_after")
    if not isinstance(before, dict) or not before.get("client_width"):
        return "void"             # nothing was there to lose
    if not rec.get("fixture_installed"):
        return "void"             # the fixture never ran: nothing was tested
    if after is None or not isinstance(after, dict) or not after.get("client_width"):
        return "canvas_lost"
    return "survived"


def fixture_counts(attempts):
    out = {"canvas_lost": 0, "survived": 0, "void": 0}
    for a in attempts:
        out[classify_fixture_attempt(a)] += 1
    return out


def fixture_verdict(attempts, expected):
    if expected < 1:
        raise DiagRefusal("expected must be >= 1")
    counts = fixture_counts(attempts)
    if counts["void"]:
        return "VOID", "%d void attempt(s)" % counts["void"], counts
    done = counts["canvas_lost"] + counts["survived"]
    if done < expected:
        return "VOID", "only %d of %d attempts completed" % (done, expected), counts
    if counts["canvas_lost"]:
        return ("FIXTURE_REMOVES_CANVAS",
                "%d/%d attempts lost the canvas to the fixture"
                % (counts["canvas_lost"], expected), counts)
    return "FIXTURE_HARMLESS", "0/%d attempts lost the canvas" % expected, counts


def run_attempt(browser, url, index, out_dir, pw_timeout_error):
    rec = {"index": index, "connected": False, "bridge_alive_after": False,
           "harness_error": None, "fixture_installed": False,
           "canvas_before": None, "canvas_after": None}
    ctx = None
    try:
        ctx = browser.new_context(viewport=dict(VIEWPORT),
                                  device_scale_factor=DEVICE_SCALE_FACTOR)
        page = ctx.new_page()
        page.add_init_script(_WS_PROBE_JS)
        page.goto(url)
        try:
            page.wait_for_selector("#status.connected", timeout=CONNECT_TIMEOUT_MS)
            rec["connected"] = True
        except pw_timeout_error:
            return rec
        page.wait_for_selector("#table-container table tbody tr",
                               timeout=CONNECT_TIMEOUT_MS)
        page.click(".tab[data-tab='waterfall']")
        try:
            page.wait_for_selector("#waterfall-chart canvas",
                                   timeout=CANVAS_TIMEOUT_MS)
        except pw_timeout_error:
            pass
        snap_before = page.evaluate(_SNAPSHOT_JS)
        rec["canvas_before"] = snap_before.get("canvas")
        rec["rows_before"] = snap_before.get("execution_rows")
        rec["lanes_before"] = snap_before.get("lanes")
        rec["sort_before"] = page.evaluate(_SORT_LABEL_JS)
        rec["host_text_before"] = snap_before.get("host_text")
        if not (isinstance(rec["canvas_before"], dict)
                and rec["canvas_before"].get("client_width")):
            rec["bridge_alive_after"] = bridge_alive(url)
            return rec

        # #514's own sequence from here on.
        if page.locator("#live-btn.active").count():
            page.click("#live-btn")
        page.evaluate(_FIXTURE_JS)
        page.click("#executions-sort-toggle")
        try:
            page.wait_for_function(
                """window.__executionFixtureError || (window.__executionFixture &&
                   document.querySelectorAll('#executions-table tbody tr.clickable').length === 100)""",
                timeout=CANVAS_TIMEOUT_MS)
        except pw_timeout_error:
            rec["fixture_wait_timed_out"] = True
        rec["fixture_error"] = page.evaluate("window.__executionFixtureError")
        rec["fixture_installed"] = bool(page.evaluate("!!window.__executionFixture"))
        rec["cloned_row"] = page.evaluate("window.__clonedRow")
        rec["sort_after"] = page.evaluate(_SORT_LABEL_JS)
        # The SAME wait #514's _wait_for_execution_list makes next.
        t0 = time.monotonic()
        try:
            page.wait_for_selector("#waterfall-chart canvas",
                                   timeout=CANVAS_TIMEOUT_MS)
        except pw_timeout_error:
            pass
        rec["canvas_wait_after_ms"] = int((time.monotonic() - t0) * 1000)
        snap_after = page.evaluate(_SNAPSHOT_JS)
        rec["canvas_after"] = snap_after.get("canvas")
        rec["rows_after"] = snap_after.get("execution_rows")
        rec["host_text_after"] = snap_after.get("host_text")
        rec["echarts_after"] = snap_after.get("echarts_instance")
        rec["detail_requests"] = [
            {k: r.get(k) for k in ("cmd", "ms", "bytes", "rows",
                                   "leader_events", "error")}
            for r in snap_after.get("requests", [])
            if r.get("cmd") in ("executions", "execution_detail")]
        if out_dir and classify_fixture_attempt(
                dict(rec, bridge_alive_after=True)) == "canvas_lost":
            shot = os.path.join(out_dir, "fixture-lost-%02d.png" % index)
            try:
                page.screenshot(path=shot)
                rec["screenshot"] = shot
            except Exception as exc:          # noqa: BLE001
                rec["screenshot_error"] = str(exc)[:200]
    except Exception as exc:                  # noqa: BLE001
        rec["harness_error"] = "%s: %s" % (type(exc).__name__, str(exc)[:300])
    finally:
        if ctx is not None:
            try:
                ctx.close()
            except Exception:                 # noqa: BLE001
                pass
    rec["bridge_alive_after"] = bridge_alive(url)
    rec["status"] = classify_fixture_attempt(rec)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--attempts", type=int, default=10)
    ap.add_argument("--phase", default="")
    ap.add_argument("--out", default=None)
    ap.add_argument("--out-dir", default=None)
    args = ap.parse_args()

    try:
        from playwright.sync_api import sync_playwright
        from playwright.sync_api import TimeoutError as PWTimeoutError
    except ImportError as exc:
        print("REFUSE: playwright is not importable (%s)" % exc, file=sys.stderr)
        sys.exit(2)
    if not bridge_alive(args.url):
        print("REFUSE: %s/session did not answer 200" % args.url.rstrip("/"),
              file=sys.stderr)
        sys.exit(2)
    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)

    attempts = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            for i in range(args.attempts):
                rec = run_attempt(browser, args.url, i, args.out_dir, PWTimeoutError)
                attempts.append(rec)
                cloned = rec.get("cloned_row") or {}
                print("[%s] %2d -> %-12s before=%s after=%s sort=%r->%r "
                      "cloned(n_events=%s n_workers=%s plan_ms=%s) host=%r"
                      % (args.phase or "-", i, rec["status"],
                         bool((rec.get("canvas_before") or {}).get("client_width")),
                         bool((rec.get("canvas_after") or {}).get("client_width")),
                         rec.get("sort_before"), rec.get("sort_after"),
                         cloned.get("n_events"), cloned.get("n_workers"),
                         cloned.get("plan_ms"),
                         (rec.get("host_text_after") or "")[:48]), flush=True)
        finally:
            browser.close()

    try:
        verdict, reason, counts = fixture_verdict(attempts, args.attempts)
    except DiagRefusal as exc:
        print("REFUSE: %s" % exc, file=sys.stderr)
        sys.exit(2)
    result = {"phase": args.phase, "url": args.url, "attempts": attempts,
              "verdict": verdict, "reason": reason, "counts": counts}
    print(json.dumps({k: v for k, v in result.items() if k != "attempts"},
                     indent=2))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
        print("wrote %s" % args.out)
    sys.exit(2 if verdict == "VOID" else 0)


if __name__ == "__main__":
    main()
