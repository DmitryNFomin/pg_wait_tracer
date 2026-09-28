#!/usr/bin/env python3
"""ui_mac_walk_safari.py -- the Safari counterpart of tests/ui_mac_walk.py:
same per-tab shape (time to first paint, console errors, rendered, freshness
at walk end) but driven through REAL Safari via safaridriver/WebDriver
(tests/safari_webdriver.py) instead of Playwright Chrome.

Safari is the browser that will actually be on stage. Chrome's paint numbers
do not transfer -- different compositor, different paint scheduling -- so an
11-tab Chrome table plus a one-tab Safari sample is not evidence about the
demo. This script produces the full 11-tab table in Safari.

REUSE (verbatim JS/data from tests/ui_live_smoke.py, same as
tests/ui_mac_walk.py already does): PANEL_CHECKS, _READY_SELECTOR,
_PANEL_ELEMENT_SELECTOR, ui_live_smoke_lib.TABS, ui_live_smoke_lib
.render_check_ok.

NOT REUSED, and why: Playwright's page.click()/wait_for_selector() have no
WebDriver equivalent used here; navigation and polling are driven through
execute_script's own DOM queries instead of WebDriver's element-finding
endpoints (POST /element, /element/{id}/click -- avoided because it roughly
doubles the HTTP round trips per action for no behavioural difference here).
ONE session serves the WHOLE walk (never a fresh session per tab like
Chrome's fresh-context-per-tab): the app is a true SPA, tab switches never
navigate, so a console-error hook installed once right after the initial
load stays valid for all 11 tabs. This also sidesteps safaridriver's session
start/stop being far slower than Playwright's context creation.

Mac attribution: gated by tests/mac_sampler.py's wait_for_quiet() before the
timed portion starts (coordinator's rule, 2026-09-28: a walk begun while the
Mac is loaded just produces another all-void run), and every tab's window is
still checked individually against ui_mac_walk_lib.window_void_reason.
majority_void() decides whether the WHOLE run counts, printed explicitly
either way -- a majority-void run is reported as not counting, never
silently reported as if the surviving tabs were the whole picture.

Screen/viewport identity: 1440x932 CSS @ DPR2 is a WINDOW, not full screen
(the owner's panel is 1710x1107 logical, avail 1710x1068, measured
2026-09-28) -- both the achieved viewport and the full screen size are
recorded in the verdict, never just assumed to match the target.

Usage:
    python3 tests/ui_mac_walk_safari.py --url http://localhost:8385/ \\
        --safaridriver-port 9994
"""
import argparse
import json
import os
import subprocess
import sys
import time
import traceback
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_mac_walk_lib as lib
import ui_live_smoke as uls        # reused JS/selector dicts only
import ui_live_smoke_lib as uls_lib
from mac_sampler import MacSampler, wait_for_quiet, check_walk_precondition
from safari_webdriver import SafariSession, WebDriverError

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "results", "ui_mac_walk_safari")

# The demo presents FULL SCREEN, not windowed -- corrected 2026-09-28 after
# the coordinator's own WebDriver measurement of the owner's actual panel:
# this Mac runs its built-in display in a SCALED mode (1710x1107 logical @
# DPR2, physical 3420x2214, downsampled to the 2880x1864 panel), so
# "physical / DPR" (the earlier 1440x932 windowed figure) undercounted by
# 19% -- a real layout difference (more table rows, different chart aspect,
# different wrapping), not a rounding error. Full-screen content viewport is
# 1710x1069 CSS @ DPR2 (38px of chrome consumed vs. 52px windowed).
# session.fullscreen() (W3C WebDriver 'Fullscreen Window'), NOT
# set_window_rect -- achieved viewport is still MEASURED after the call and
# ASSERTED against this target below, never assumed.
TARGET_VIEWPORT = {"width": 1710, "height": 1069, "dpr": 2}

# Pinned criteria for this branch's counted attempt (coordinator,
# 2026-09-28): docs/DEMO_REHEARSAL_CRITERIA.md as of commit f094280 on
# agent/criteria-demo-config, doc blob 758302f. Recorded in every verdict
# alongside this branch's own commit SHA (captured at runtime below) --
# "that is what makes an attempt countable: the criteria cannot move under
# it afterwards."
CRITERIA_COMMIT = "f094280568be2dafa0122551519dd2789575b3cd"
CRITERIA_DOC_BLOB = "758302f9af3a424abd7eb7793a2cca12d9a9226b"


def _current_branch_sha():
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                            text=True, cwd=os.path.dirname(os.path.abspath(__file__)))
        return r.stdout.strip() if r.returncode == 0 else None
    except Exception:
        return None


def _blocked_verdict(run_id, blocked_info):
    """Shared shape for every early-refusal verdict (precondition,
    fullscreen(), viewport assertion) -- always carries the pinned
    criteria identity and attempt_counted=False, same fields as a
    completed run's verdict, so a reader never has to guess why one path's
    JSON looks different from another's."""
    return {
        "run": {"id": run_id}, "ok": False, "counted": False,
        "attempt_counted": False,
        "criteria_commit": CRITERIA_COMMIT,
        "criteria_doc_blob": CRITERIA_DOC_BLOB,
        "branch_sha": _current_branch_sha(),
        "blocked": blocked_info,
    }

CONSOLE_HOOK_JS = """
window.__macWalkErrors = [];
(function () {
    const origError = console.error.bind(console);
    console.error = function (...args) {
        try { window.__macWalkErrors.push('console: ' + args.map(String).join(' ')); }
        catch (e) {}
        return origError(...args);
    };
    window.addEventListener('error', function (e) {
        window.__macWalkErrors.push('pageerror: ' +
            ((e.error && e.error.stack) || e.message || String(e)));
    });
    window.addEventListener('unhandledrejection', function (e) {
        const r = e.reason;
        window.__macWalkErrors.push('Unhandled rejection: ' +
            ((r && (r.stack || r.message)) || String(r)));
    });
})();
"""


def _is_pgwt_error(e):
    return e.startswith("console: [pgwt]")


def drain_console_errors(session):
    """Reads + clears window.__macWalkErrors in one atomic execute_script
    call (no gap for an error landing between a read and a separate
    clear)."""
    return session.execute_script(
        "const errs = window.__macWalkErrors || []; "
        "window.__macWalkErrors = []; return errs;")


def wait_for_js(session, bool_expr, timeout_s, poll_s=0.15):
    """Polls `return (bool_expr)` until true. Returns elapsed_s, or None on
    timeout (never fabricated)."""
    start = time.monotonic()
    deadline = start + timeout_s
    while True:
        if session.execute_script(f"return !!({bool_expr})"):
            return time.monotonic() - start
        if time.monotonic() >= deadline:
            return None
        time.sleep(poll_s)


# Documented EMPTY-state markers (2026-09-28, review correction): waterfall
# and scatter render a ".loading" div with a specific message when the
# trace window has no completed executions -- no canvas is ever created for
# that state BY DESIGN (waterfall.js:215 "No executions for selected
# range", exec-scatter.js:70 "No completed, positive-duration executions
# for selected range"). _READY_SELECTOR alone (from ui_live_smoke.py,
# "#waterfall-chart canvas" / "#scatter-chart canvas") can never match that
# state, so a legitimately empty panel used to time out and read as a
# render FAILURE. Two different containers: waterfall's empty marker lands
# in #executions-table (waterfall.js:215), not #waterfall-chart itself;
# scatter's lands in its own #scatter-chart container (exec-scatter.js:70).
_EMPTY_STATE_SELECTOR = {
    "waterfall": "#executions-table .loading",
    "scatter": "#scatter-chart .loading",
}


def navigate_to_tab(session, tab_id, timeout_s):
    """WebDriver analogue of ui_live_smoke.py's _navigate_to_tab -- same
    Timeline drill-in special case, same #live-btn resume idiom, driven
    through execute_script instead of Playwright element handles.

    Returns 'data' or 'empty' -- WHICH documented state the panel reached
    (see _EMPTY_STATE_SELECTOR), so a legitimately empty panel is never
    conflated with one that actually failed to render."""
    if tab_id == "timeline":
        ok = session.execute_script(
            "const b = document.querySelector(\".tab[data-tab='sessions']\"); "
            "if (!b) return false; b.click(); return true;")
        if not ok:
            raise RuntimeError("no Sessions tab button to drill into Timeline from")
        if wait_for_js(session, "document.querySelector("
                        "'#table-container table tbody tr')", timeout_s) is None:
            raise RuntimeError("Sessions table never populated before Timeline drill")
        clicked = session.execute_script(
            "const row = document.querySelector("
            "'#table-container table tbody tr.clickable'); "
            "if (!row) return false; row.click(); return true;")
        if not clicked:
            raise RuntimeError("no clickable session row to drill into for Timeline")
    else:
        ok = session.execute_script(
            f"const b = document.querySelector(\".tab[data-tab='{tab_id}']\"); "
            "if (!b) return false; b.click(); return true;")
        if not ok:
            raise RuntimeError(f"no tab button for {tab_id!r}")

    ready_sel = json.dumps(uls._READY_SELECTOR[tab_id])
    empty_sel = _EMPTY_STATE_SELECTOR.get(tab_id)
    if empty_sel:
        empty_sel_json = json.dumps(empty_sel)
        condition = (f"document.querySelector({ready_sel}) || "
                     f"document.querySelector({empty_sel_json})")
    else:
        empty_sel_json = None
        condition = f"document.querySelector({ready_sel})"
    if wait_for_js(session, condition, timeout_s) is None:
        extra = f" (no empty-state marker {empty_sel!r} either)" if empty_sel else ""
        raise RuntimeError(f"panel did not render ({uls._READY_SELECTOR[tab_id]!r}) "
                            f"within {timeout_s}s{extra}")

    render_state = "data"
    if empty_sel_json and session.execute_script(
            f"return document.querySelector({empty_sel_json}) !== null"):
        render_state = "empty"

    live_active = session.execute_script(
        "const b = document.getElementById('live-btn'); "
        "return !!b && b.classList.contains('active');")
    if not live_active:
        clicked = session.execute_script(
            "const b = document.getElementById('live-btn'); "
            "if (!b) return false; b.click(); return true;")
        if not clicked:
            raise RuntimeError("no #live-btn to resume live mode")
        time.sleep(0.5)
        if not session.execute_script(
                "const b = document.getElementById('live-btn'); "
                "return !!b && b.classList.contains('active');"):
            raise RuntimeError("clicking #live-btn did not resume live mode")

    return render_state


def empty_state_text(session, tab_id):
    """The actual text of the documented empty-state marker (2026-09-28
    review: "record which state it saw" -- not just that it was empty, but
    what it said), or None if this tab has no documented empty state."""
    sel = _EMPTY_STATE_SELECTOR.get(tab_id)
    if not sel:
        return None
    return session.execute_script(
        f"const el = document.querySelector({json.dumps(sel)}); "
        "return el ? el.textContent.trim() : null;")


def render_check(session, tab_id, timeout_s=2.0, poll_s=0.15):
    deadline = time.monotonic() + timeout_s
    while True:
        raw = session.execute_script(f"return ({uls.PANEL_CHECKS[tab_id]})();")
        ok, detail = uls_lib.render_check_ok(raw)
        if ok or time.monotonic() >= deadline:
            return ok, detail
        time.sleep(poll_s)


def screen_identity(session):
    return session.execute_script(
        "return {viewport: {width: window.innerWidth, height: window.innerHeight, "
        "dpr: window.devicePixelRatio}, "
        "screen: {width: screen.width, height: screen.height, "
        "availWidth: screen.availWidth, availHeight: screen.availHeight}};")


def version_skew_snapshot(session):
    return session.execute_script(
        "const el = document.getElementById('version-skew'); "
        "if (!el) return {present: false}; "
        "const cs = getComputedStyle(el); "
        "return {present: true, visible: cs.display !== 'none', "
        "text: el.textContent, cls: el.className};")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True)
    ap.add_argument("--safaridriver-port", type=int, default=9994)
    ap.add_argument("--out-dir", default=RESULTS_DIR)
    ap.add_argument("--first-data-timeout", type=float, default=uls_lib.FIRST_DATA_TIMEOUT_S)
    ap.add_argument("--max-quiet-wait-s", type=float, default=300.0,
                     help="bounded pre-flight wait for Mac pressure_level==1 "
                          "before the timed walk starts (default 300s)")
    ap.add_argument("--skip-quiet-wait", action="store_true",
                     help="skip the pre-flight quiet check (the walk's own "
                          "per-tab void accounting still applies either way)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    for name in os.listdir(args.out_dir):
        p = os.path.join(args.out_dir, name)
        if os.path.isdir(p):
            import shutil
            shutil.rmtree(p)
        else:
            os.remove(p)

    run_id = os.environ.get("PGWT_RUN_MARKER", str(int(time.time())))
    with open(os.path.join(args.out_dir, "run.id"), "w") as f:
        f.write(run_id)

    # Root-caused live (2026-09-28, coordinator's own `ioreg`/
    # `system_profiler` measurement, corroborated independently here):
    # session.fullscreen() (POST /session/{id}/window/fullscreen) fails
    # with an opaque "unknown error" when the console session is LOCKED
    # (ioreg -n Root -d1 -a -> IOConsoleUsers[0].CGSSessionScreenIsLocked)
    # -- macOS refuses a fullscreen Space transition on a locked session,
    # full stop. Display-asleep is downstream of the same cause (the
    # display sleeps, then the session locks) but checked on its own too.
    # Asserted HERE, before spawning safaridriver/creating a Safari
    # session at all, and FAILS LOUDLY WITHOUT RETRYING -- "an error naming
    # the real cause is worth more than five minutes of opaque retries"
    # (coordinator). Earlier iterations of this script tried mitigations
    # that turned out insufficient (waking the display via caffeinate,
    # waiting out an animation-settle window) BEFORE this was root-caused;
    # those are kept (caffeinate -d below is still a real, ordinary
    # mitigation for the display-asleep sub-case) but this assertion is
    # the actual fix -- it stops the walk from ever reaching the point
    # where it would fail opaquely.
    precondition_reason, screen_locked, display_asleep = check_walk_precondition()
    print(f"pre-flight: screen_locked={screen_locked} display_asleep={display_asleep}")
    if precondition_reason is not None:
        print(f"ERROR: refusing to start the Safari walk: {precondition_reason}",
              file=sys.stderr)
        verdict = _blocked_verdict(run_id, {
            "error": "walk precondition not met",
            "reason": precondition_reason,
            "screen_locked": screen_locked,
            "display_asleep": display_asleep})
        with open(os.path.join(args.out_dir, "summary.json"), "w") as f:
            json.dump(verdict, f, indent=2, sort_keys=True)
        return 1

    if not args.skip_quiet_wait:
        print(f"pre-flight: waiting up to {args.max_quiet_wait_s}s for the "
              "Mac to go quiet (pressure_level == 1) before starting the timed walk")
        quiet, last_sample = wait_for_quiet(args.max_quiet_wait_s)
        print(f"pre-flight: quiet={quiet} last_sample={last_sample}")
    else:
        print("pre-flight: --skip-quiet-wait given, starting immediately")

    # Hold the display awake for the whole walk. `caffeinate -d` (prevent
    # display idle sleep, no -t so it runs until killed) is ordinary,
    # non-privileged tooling, not an interactive/security grant --
    # explicitly distinct from the `safaridriver --enable` category this
    # walk must never attempt itself.
    #
    # ROOT-CAUSED AND SETTLED (2026-09-28). session.fullscreen()'s opaque
    # "unknown error" is macOS refusing a fullscreen Space transition while
    # the console session is LOCKED (CGSSessionScreenIsLocked) -- and THE
    # LOCK IS SUFFICIENT, THE ONLY CAUSE, no third explanation needed.
    # Bisected properly before settling this: 0 fullscreen failures across
    # 7 attempts (plain WebDriver calls and this module's full
    # reset_zoom()+settle-polling wrapper, both foreground and backgrounded)
    # whenever CGSSessionScreenIsLocked read False at call time; 100%
    # failure across 4 attempts -- including the coordinator's own minimal
    # 6-step repro with none of this module's extras -- whenever it read
    # True, re-checked at the moment of failure each time. Two earlier,
    # now-retracted theories (display sleep alone; an unidentified further
    # cause) were both artifacts of checking the lock state too early or
    # not at all -- display sleep is genuinely downstream of the same
    # cause (the display sleeps, then the session locks), never an
    # independent trigger. The fix is the check_walk_precondition()
    # refusal above: check BOTH facts, refuse outright, never reach this
    # point if either is true. `caffeinate -d` is kept only as an
    # ordinary, real mitigation for the display-asleep sub-case
    # specifically -- it cannot prevent or detect the lock, which is a
    # separate macOS setting this walk does not and must not try to work
    # around itself.
    caffeinate_proc = subprocess.Popen(["caffeinate", "-d"])

    driver_log = os.path.join(args.out_dir, "safaridriver.log")
    open(driver_log, "w").close()
    driver_proc = subprocess.Popen(
        ["/usr/bin/safaridriver", "--port", str(args.safaridriver_port)],
        stdout=open(driver_log, "ab"), stderr=subprocess.STDOUT)
    driver_url = f"http://localhost:{args.safaridriver_port}"
    deadline = time.monotonic() + 15
    ready = False
    while time.monotonic() < deadline:
        if driver_proc.poll() is not None:
            break
        try:
            with urllib.request.urlopen(f"{driver_url}/status", timeout=2) as r:
                if r.status == 200:
                    ready = True
                    break
        except Exception:
            pass
        time.sleep(0.5)
    if not ready:
        print(f"ERROR: safaridriver never became ready on port {args.safaridriver_port}",
              file=sys.stderr)
        if driver_proc.poll() is None:
            driver_proc.kill()
        if caffeinate_proc.poll() is None:
            caffeinate_proc.kill()
        return 1

    # Found live (2026-09-28): the default 10s SAMPLE_INTERVAL_S (tuned for
    # tests/ui_mac_walk.py's slower Playwright-per-tab-context walk) is
    # coarser than this module's own walk -- driving through execute_script
    # DOM polling instead of a fresh browser context per tab, all 11 tabs'
    # TTFP pass finished in ~5s on a real run -- so most/every tab's window
    # fell entirely between two samples and window_void_reason correctly
    # (by its own fail-closed contract) reported "no samples in window",
    # even with the Mac genuinely idle (0/3 samples non-normal that run).
    # A shorter interval here is the fix, not loosening the attribution
    # rule itself.
    # Found live (2026-09-28), second measurement: 2.0s still left 9/11 tabs
    # void (0/11 samples non-normal both times -- a genuine sampling-density
    # gap, not real Mac pressure). Each tab's own TTFP-pass window is
    # ~0.2-0.9s; 0.5s gives most windows a real chance at a sample.
    #
    # MEASURED per-read cost (2026-09-28, this Mac, n=20 calls to
    # MacSampler.read_once() -- sysctl + memory_pressure -Q + os.getloadavg
    # combined): min 3.1ms, max 4.2ms, mean 3.4ms -- NOT the "a few ms" this
    # comment used to say without a number behind it, and also NOT the
    # ~1.5s/read a review pass reported measuring separately (which would
    # push a 0.5s-configured interval out to ~2.02s actual). Corroborated
    # from a real run's own recorded mac_samples (run10, 53 samples): mean
    # inter-sample gap 0.513s, max 0.529s -- the configured 0.5s interval
    # was respected within ~6%, not stretched to ~2s. This discrepancy is
    # unresolved and reported as such rather than silently trusting either
    # number; window_void_reason's max_gap_s bound below is derived from
    # THIS run's own sample_cadence_stats() regardless of which figure is
    # right, so the bound is self-correcting per run even if the
    # discrepancy's cause is never found.
    sampler = MacSampler(interval_s=0.5)
    sampler.start()

    tabs = []
    notes = []
    version_skew = None
    display_identity = None
    tick_wait_elapsed_s = None
    tick_wait_bound_s = None
    session = None
    try:
        try:
            session = SafariSession(driver_url)
        except WebDriverError as e:
            print(f"BLOCKED: SafariSession could not open a session: {e}",
                  file=sys.stderr)
            verdict = _blocked_verdict(run_id, {"error": str(e), "raw": e.raw})
            with open(os.path.join(args.out_dir, "summary.json"), "w") as f:
                json.dump(verdict, f, indent=2, sort_keys=True)
            return 1

        # ORDER MATTERS, confirmed independently twice (coordinator's own
        # minimal script, and this script's own opaque-error history):
        # create session -> NAVIGATE -> (short settle) -> fullscreen().
        # Calling fullscreen() immediately after session creation with no
        # navigation yet returns the same opaque HTTP 500 "unknown error"
        # this module already works around for the locked-session case --
        # a page must be loaded and focused before a Space transition is
        # granted at all. Never reorder this.
        session.navigate(args.url)
        # Safari persists a PER-HOSTNAME page zoom, invisible and permanent
        # until cleared -- measured 2026-09-28 (coordinator): ~115% for the
        # hostname "localhost" specifically on this Mac (127.0.0.1 and
        # about:blank both unaffected), which silently shrank the achieved
        # fullscreen viewport by the same 1440->1252 / 1710->1486 factor
        # (0.8695, i.e. 1/1.15) on two separate real runs. The harness must
        # SET the zoom, never inherit whatever Safari remembers for this
        # hostname -- that is exactly the invisible per-machine state that
        # makes a rehearsal unreproducible. reset_zoom() (Cmd+0) is the only
        # lever WebDriver exposes (no W3C "set zoom" endpoint); its actual
        # effect is verified below by comparing the achieved viewport
        # against TARGET_VIEWPORT regardless -- this call is a mitigation
        # attempt, the assertion after fullscreen() is the real gate.
        session.reset_zoom()
        try:
            session.fullscreen()
            achieved = session.viewport_size()
        except WebDriverError as e:
            # Found live (2026-09-28): safaridriver's own
            # 'POST .../window/fullscreen' can fail with an opaque
            # "unknown error" (empty message) independent of this script or
            # the app under test -- reproduced against a trivial page
            # (example.com), with the SAME session's set_window_rect()
            # working normally right before it. Root-caused (coordinator's
            # ioreg/system_profiler measurement): macOS refuses a
            # fullscreen Space transition while the console session is
            # LOCKED (CGSSessionScreenIsLocked) -- check_walk_precondition()
            # above should have refused BEFORE reaching here, so a failure
            # at this point most likely means the session re-locked mid-walk
            # (recorded below via the SAME precondition check, re-read now,
            # not assumed still true from the pre-flight read). Caught here
            # (not left to propagate as a bare traceback) so a real run
            # still produces a verdict.json a caller can read, with the
            # exact WebDriver error preserved verbatim -- never re-diagnosed
            # or guessed at.
            reason_now, locked_now, asleep_now = check_walk_precondition()
            print(f"ERROR: session.fullscreen() failed: {e} "
                  f"(re-checked precondition: locked={locked_now} asleep={asleep_now})",
                  file=sys.stderr)
            verdict = _blocked_verdict(run_id, {
                "error": "fullscreen() failed",
                "webdriver_error": str(e), "raw": e.raw,
                "screen_locked_now": locked_now,
                "display_asleep_now": asleep_now})
            with open(os.path.join(args.out_dir, "summary.json"), "w") as f:
                json.dump(verdict, f, indent=2, sort_keys=True)
            return 1

        # Effective zoom factor, recorded regardless of whether
        # reset_zoom() actually worked -- coordinator: "if WebDriver cannot
        # set Safari's page zoom directly, say so and record the measured
        # zoom factor (target / achieved) so at least the number is
        # explicit rather than hidden."
        effective_zoom = lib.effective_zoom(TARGET_VIEWPORT["width"], achieved.get("width"))

        if achieved != TARGET_VIEWPORT:
            # Fail loudly (coordinator's explicit instruction), never
            # silently proceed at whatever viewport fullscreen() happened
            # to produce -- a walk graded against the wrong viewport is
            # worse than no walk.
            print(f"ERROR: fullscreen viewport {achieved} != target "
                  f"{TARGET_VIEWPORT} (effective_zoom={effective_zoom}) -- "
                  "refusing to run the walk at the wrong viewport",
                  file=sys.stderr)
            verdict = _blocked_verdict(run_id, {
                "error": "viewport assertion failed",
                "achieved": achieved, "target": TARGET_VIEWPORT,
                "effective_zoom": effective_zoom})
            with open(os.path.join(args.out_dir, "summary.json"), "w") as f:
                json.dump(verdict, f, indent=2, sort_keys=True)
            return 1

        try:
            if wait_for_js(session, "document.getElementById('status') && "
                           "document.getElementById('status').classList.contains"
                           "('connected')", args.first_data_timeout) is None:
                raise RuntimeError("never reached #status.connected initially")
            deadline2 = time.monotonic() + args.first_data_timeout
            while session.execute_script(
                    "return document.querySelector("
                    "'#table-container table tbody tr') === null"):
                if time.monotonic() > deadline2:
                    raise RuntimeError(f"UI showed no data within {args.first_data_timeout}s")
                time.sleep(0.2)

            session.execute_script(CONSOLE_HOOK_JS)
            drain_console_errors(session)  # discard anything from before the hook
            # Install once, right after the first connect, so it
            # accumulates ticks across the WHOLE session (true SPA, tab
            # switches never navigate) -- not freshness-pass-local.
            #
            # ROOT-CAUSED AND FIXED (2026-09-28, review): the previous
            # `session.execute_script(uls.TICK_HOOK_JS)` was the entire
            # freshness story -- four consecutive "unmeasured" results were
            # a deterministic install failure, not the walk outrunning the
            # product. WebDriver's execute/sync posts `script` as a W3C
            # FUNCTION BODY (wrapped internally as `function(args){
            # <script> }`), but uls.TICK_HOOK_JS is written as
            # `"() => { ... }"` -- an arrow function EXPRESSION, not
            # statements. Posted bare, that body just constructs an
            # anonymous function object and immediately discards it; the
            # assignment to window.__uiLiveTicks inside it never runs.
            # Proof: `session.execute_script("return typeof
            # window.__uiLiveTicks")` read "undefined" right after the
            # unwrapped call. CONSOLE_HOOK_JS above is written as bare
            # statements (no arrow-function wrapper), which is exactly why
            # it worked while this did not. Fix: wrap it as an IIFE so the
            # posted body both defines AND calls it.
            session.execute_script(f"({uls.TICK_HOOK_JS})()")
            if session.execute_script("return typeof window.__uiLiveTicks") == "undefined":
                raise RuntimeError(
                    "TICK_HOOK_JS install verification failed: "
                    "window.__uiLiveTicks is still undefined after the IIFE-wrapped "
                    "install -- fail loudly rather than silently measuring nothing again")
            # Found live (2026-09-28), superseded by the fix above (kept for
            # the record): with the hook APPEARING installed but silently
            # inert, EVERY freshness read still came back None -- this
            # module's whole 11-tab walk finishes in ~5-8s (execute_script
            # DOM polling, no
            # fresh browser context per tab like tests/ui_mac_walk.py), so
            # it can outrun app.js's own 5s AAS auto-refresh cadence
            # entirely: the freshness pass ran before even ONE periodic
            # tick had fired since the hook was installed. First fix tried
            # an 8s bound here and STILL timed out on the next real run
            # (0 ticks observed) -- 8s is under 2x the 5s cadence, not
            # enough margin for jitter/startup. Chosen fix, decided
            # 2026-09-28 (of the two options: wait longer up front, vs.
            # accept a possibly-one-tick-stale read at walk end): wait
            # longer up front, because "accept a stale read" still has
            # nothing to read at all if zero ticks have fired by the time
            # the freshness pass runs -- it does not fix the race, it just
            # relabels the same None. Bound raised to 3x TICK_S (15s,
            # ui_mac_walk_lib.TICK_S -- the SAME constant app.js's own
            # cadence is written against, not an independent guess); the
            # actual wait outcome (elapsed time, or 'timed out') is
            # recorded in the verdict either way -- never a fabricated
            # freshness reading, and this is the only place that needs the
            # wait (once at least one tick exists, every later 'age since
            # last tick' read is meaningful).
            tick_wait_bound_s = lib.TICK_S * 3
            tick_wait_elapsed_s = wait_for_js(
                session, "window.__uiLiveTicks && "
                "window.__uiLiveTicks.length >= 1", tick_wait_bound_s)
            if tick_wait_elapsed_s is None:
                print(f"  note: no AAS tick observed within {tick_wait_bound_s}s of "
                      "installing the tick hook -- freshness will read as unmeasured",
                      file=sys.stderr)
            display_identity = screen_identity(session)
            display_identity["viewport_matches_target"] = (
                display_identity["viewport"] == TARGET_VIEWPORT)
            display_identity["effective_zoom"] = effective_zoom
            display_identity["safari_browser_version"] = session.capabilities.get("browserVersion")
            display_identity["safari_platform_build"] = session.capabilities.get(
                "safari:platformBuildVersion")
            display_identity["macos_product_version"] = subprocess.run(
                ["sw_vers", "-productVersion"], capture_output=True, text=True).stdout.strip()
            display_identity["macos_build_version"] = subprocess.run(
                ["sw_vers", "-buildVersion"], capture_output=True, text=True).stdout.strip()
            print(f"display identity: {display_identity}")

            version_skew = lib.version_skew_from_snapshot(version_skew_snapshot(session))
            print(f"version_skew: {version_skew}")

            for tab_id in uls_lib.TABS:
                print(f"=== {tab_id} ===")
                wall_start = time.time()
                t0 = time.monotonic()
                render_ok, render_detail, ttfp_s, render_state = False, "never checked", None, None
                try:
                    render_state = navigate_to_tab(session, tab_id, args.first_data_timeout)
                    ttfp_s = time.monotonic() - t0
                    if render_state == "empty":
                        # A documented empty state (2026-09-28 review
                        # correction) is a legitimate, working panel --
                        # PANEL_CHECKS (ui_live_smoke.py, read-only here)
                        # would report ok:False for it (it checks for
                        # rows/series data, which an empty result has
                        # none of by design), so it is deliberately NOT
                        # consulted for this state; render_ok is set True
                        # directly and the actual empty-state message is
                        # recorded verbatim in render_detail.
                        render_ok = True
                        render_detail = f"empty: {empty_state_text(session, tab_id)}"
                    else:
                        render_ok, render_detail = render_check(session, tab_id)
                except Exception as e:
                    render_detail = str(e)
                    print(f"  FAIL [{tab_id}]: {e}", file=sys.stderr)

                raw_errors = drain_console_errors(session)
                console_errors, pgwt_errors = [], []
                for e in raw_errors:
                    (pgwt_errors if _is_pgwt_error(e) else console_errors).append(e)

                shot_path = None
                try:
                    sel = uls._PANEL_ELEMENT_SELECTOR.get(tab_id)
                    if sel and session.execute_script(
                            f"return document.querySelector({json.dumps(sel)}) !== null"):
                        tab_dir = os.path.join(args.out_dir, tab_id)
                        os.makedirs(tab_dir, exist_ok=True)
                        shot_path = os.path.join(tab_dir, "ttfp.png")
                        session.screenshot(shot_path)
                except Exception as e:
                    print(f"  note [{tab_id}]: could not capture screenshot: {e}",
                          file=sys.stderr)

                wall_end = time.time()
                # Void attribution is NOT computed here -- found live
                # (2026-09-28): bracketing needs a sample AT OR AFTER
                # wall_end, which by definition cannot exist yet the
                # instant wall_end is set (the sampler's next tick is up
                # to interval_s in the FUTURE relative to right now). Every
                # tab voided with "no sample after this window's end" on a
                # verifiably quiet run for exactly this reason -- a
                # causality bug in WHEN the check ran, not in the
                # bracketing formula itself. wall_start/wall_end are
                # recorded now; void_reason/brackets are computed
                # retroactively below, once the walk (and its sampler) has
                # had time to move past every tab's own window.
                entry = lib.build_tab_entry(
                    tab_id, render_ok, render_detail, ttfp_s, console_errors,
                    pgwt_errors, freshness_age_s=None, mac_void_reason=None,
                    wall_start=wall_start, wall_end=wall_end,
                    render_state=render_state)
                tabs.append(entry)
                print(f"  rendered={entry['rendered']} render_state={render_state!r} "
                      f"ttfp_s={ttfp_s} console_errors={len(console_errors)} "
                      "(void attribution pending)")

            print("=== freshness pass (revisit each tab, same session) ===")
            for entry in tabs:
                tab_id = entry["tab"]
                try:
                    navigate_to_tab(session, tab_id, args.first_data_timeout)
                    time.sleep(0.3)
                    last_ts = session.execute_script(
                        "return window.__uiLiveTicks && window.__uiLiveTicks.length "
                        "? window.__uiLiveTicks[window.__uiLiveTicks.length - 1] : null;")
                    now_ms = session.execute_script("return Date.now();")
                    age_s = (now_ms - last_ts) / 1000.0 if last_ts is not None else None
                except Exception as e:
                    print(f"  note [freshness {tab_id}]: {e}", file=sys.stderr)
                    age_s = None
                fresh_ok, fresh_detail = lib.freshness_ok(age_s)
                entry["freshness"]["age_s"] = age_s
                entry["freshness"]["ok"] = fresh_ok
                entry["freshness"]["status"] = lib.freshness_status(age_s)
                entry["freshness"]["detail"] = fresh_detail
                print(f"  freshness [{tab_id}]: {fresh_detail}")

            # RETROACTIVE void attribution, now that the freshness pass has
            # given the sampler time to tick past every main-loop tab's own
            # wall_end. max_gap_s is derived from this run's OWN measured
            # sample cadence (sample_cadence_stats), never the configured
            # interval -- 2026-09-28 review: a subprocess-based sampler on
            # this Mac is not guaranteed to keep its configured interval
            # (measured elsewhere on this branch: ~1.5s/read under load
            # pushed a 0.5s-configured cadence out to ~2.02s actual), so a
            # bound picked from the config would be a fiction. 3x the
            # MEASURED max gap is the bound: generous enough to absorb one
            # slow read without false-voiding a genuinely bracketed window,
            # tight enough that two calm readings either side of real load
            # (the review's own reproduction: two normal samples ~2 minutes
            # apart certifying everything between them) still voids.
            print("=== retroactive void attribution (bracketing against "
                  "the final sample set) ===")
            final_samples = list(sampler.samples)
            mean_gap, max_gap = lib.sample_cadence_stats(final_samples)
            if max_gap is None:
                max_gap_s = 0.0  # fewer than 2 samples -- nothing can bracket
                print(f"  note: fewer than 2 mac samples recorded ({len(final_samples)}) "
                      "-- every window will void")
            else:
                max_gap_s = max_gap * 3
                print(f"  measured sample cadence: mean={mean_gap:.3f}s "
                      f"max={max_gap:.3f}s over {len(final_samples)} samples "
                      f"-> max_gap_s bound={max_gap_s:.3f}s")
            for entry in tabs:
                reason = lib.window_void_reason(
                    final_samples, entry["wall_start"], entry["wall_end"], max_gap_s)
                before, after = lib.bracketing_samples(
                    final_samples, entry["wall_start"], entry["wall_end"])
                entry["void_reason"] = reason
                entry["bracket_before"] = before
                entry["bracket_after"] = after
                # ok recomputed here, not left as whatever build_tab_entry
                # set at construction time (freshness wasn't measured yet
                # then) -- MUST include freshness_status == "ok" (review
                # blocker, 2026-09-28): a verdict aggregated to ok: true
                # once with freshness unmeasured on every tab.
                entry["ok"] = (bool(entry["rendered"]) and not entry["console_errors"]
                               and reason is None
                               and entry["freshness"]["status"] == "ok")
                print(f"  [{entry['tab']}] void={reason!r} "
                      f"freshness_status={entry['freshness']['status']!r} "
                      f"ok={entry['ok']}")
        except Exception:
            traceback.print_exc()
    finally:
        sampler.stop()
        if session is not None:
            session.close()
        if driver_proc.poll() is None:
            driver_proc.kill()
        if caffeinate_proc.poll() is None:
            caffeinate_proc.kill()

    notes.append("freshness is a page-global AAS-tick-age proxy, not a per-chart-type "
                 "newest-bucket read -- see tests/ui_mac_walk.py's module docstring "
                 "for the same tradeoff there.")
    notes.append("window.__uiLiveTicks is installed inline via execute_script, not "
                 "an init script (WebDriver has no add_init_script equivalent used "
                 "here) -- installed once right after first load, valid for the "
                 "whole SPA session since tab switches never navigate.")
    verdict = lib.build_verdict(run_id, tabs, sampler.samples, notes=notes)
    verdict["version_skew"] = version_skew
    verdict["display_identity"] = display_identity
    # Same environment-fact family as memory_pressure/loadavg (mac_samples
    # above): the pre-flight reading, taken once before the timed walk
    # started (not re-polled per tick -- the walk refuses outright rather
    # than starting if either was true, so by construction both are False/
    # False for every run that reaches this point).
    verdict["pre_flight_screen_locked"] = screen_locked
    verdict["pre_flight_display_asleep"] = display_asleep
    verdict["criteria_commit"] = CRITERIA_COMMIT
    verdict["criteria_doc_blob"] = CRITERIA_DOC_BLOB
    verdict["branch_sha"] = _current_branch_sha()
    # The pre-tab-walk wait for the first AAS tick (see the tick-hook
    # install site above): bound and actual elapsed/timeout outcome,
    # recorded either way so a reader can see whether freshness had a
    # real chance to be measured this run, not just whether it was.
    verdict["tick_wait_bound_s"] = tick_wait_bound_s
    verdict["tick_wait_elapsed_s"] = tick_wait_elapsed_s
    # counted, precondition-only (coordinator, 2026-09-28): gated strictly
    # on the four assertions already enforced above by construction (a run
    # that reached this point already passed unlocked/awake at pre-flight,
    # and viewport==TARGET_VIEWPORT with effective_zoom==1.0 or it would
    # have returned 1 already) -- NOT on the walk being perfect (tab
    # rendering, console errors, void count are separate facts, reported
    # but not gating this field).
    verdict["attempt_counted"] = (
        screen_locked is False and display_asleep is False
        and display_identity is not None
        and display_identity.get("viewport") == TARGET_VIEWPORT
        and display_identity.get("effective_zoom") == 1.0)
    verdict_path = os.path.join(args.out_dir, "summary.json")
    with open(verdict_path, "w") as f:
        json.dump(verdict, f, indent=2, sort_keys=True)

    assert lib.run_id_matches(verdict, run_id), "run.id mismatch immediately after writing -- bug"

    print()
    print("════════════════════════════════════════")
    print("  SAFARI MAC WALK SUMMARY")
    print("════════════════════════════════════════")
    for tab_id in uls_lib.TABS:
        e = verdict["tabs"].get(tab_id)
        if e is None:
            print(f"  MISSING {tab_id} (walk aborted before reaching it)")
            continue
        label = "VOID" if e["void_reason"] else ("PASS" if e["ok"] else "FAIL")
        print(f"  {label:5s} {tab_id:12s} ttfp={e['ttfp_s']} "
              f"rendered={e['rendered']} console_errors={len(e['console_errors'])} "
              f"freshness={e['freshness']}")
    print(f"  mac samples: {len(sampler.samples)} "
          f"(non-normal: {sum(1 for s in sampler.samples if not lib.sample_is_normal(s))})")
    print(f"  void_count={verdict['void_count']}/{verdict['tab_count']} "
          f"counted={verdict['counted']}")
    if not verdict["counted"]:
        print("  THIS RUN DOES NOT COUNT: majority of tabs were void "
              "(Mac-side infrastructure noise) -- see mac_samples above")
    print(f"  version_skew: {verdict.get('version_skew')}")
    print(f"  display_identity: {verdict.get('display_identity')}")
    print(f"  summary: {verdict_path}")
    print(f"  overall ok: {verdict['ok']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
