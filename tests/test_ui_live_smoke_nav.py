#!/usr/bin/env python3
"""test_ui_live_smoke_nav.py -- issue #245 (TTFP): unit tests for
tests/ui_live_smoke.py's `_navigate_to_tab`, specifically the ttfp_ms it now
returns (elapsed ms from this navigation's own landing click to the
ViewManager mount chokepoint's first FRESH mount of the tab -- see
_navigate_to_tab's own docstring).

Needs Playwright importable (ui_live_smoke.py hard-requires it at import
time) -- unlike tests/ui_live_smoke_lib.py's own unit tests, this file is
NOT in tests/unit_tests.list (that list's build-and-unit CI job installs no
Playwright on purpose -- see test_demo_workload_coverage.py's own
test_import_needs_no_playwright). It runs instead as its own step in
scripts/check.sh's full (non-fast) tier, which already requires Playwright
before it runs anything.

No browser, no I/O: `_navigate_to_tab` is exercised against a hand-written
fake Page object (duck-typing exactly the Playwright Page surface it calls)
so this stays a fast, deterministic unit test, same idiom as
tests/test_ui_live_smoke_lib.py.

Usage: python3 tests/test_ui_live_smoke_nav.py
"""
import ast
import inspect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke as smoke
import ui_live_smoke_lib as lib

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


class _FakeRow:
    """Stands in for the Playwright ElementHandle returned by
    page.query_selector for the clickable session row -- logs its click
    into the SAME page.calls list (via the page it was handed), so a test
    can see exactly where the row click falls relative to every other call."""

    def __init__(self, page, tag):
        self._page = page
        self._tag = tag

    def click(self):
        self._page.calls.append(("row_click", self._tag))


class _FakePage:
    """Duck-types only the Page surface _navigate_to_tab actually calls:
    click / wait_for_selector / query_selector / wait_for_timeout /
    evaluate. Every call is appended, IN ORDER, to self.calls -- the whole
    point of this fake is to let a test assert WHEN the ttfp_ms anchor
    (the "Date.now()" evaluate call) lands relative to the clicks around
    it, not just what value comes out.

    mount: the {"id", "seq", "at"} dict returned for window.__pgwt.viewMount()
    (VIEW_MOUNT_JS) -- pre-set so _wait_for_mount_at_or_after's very first
    poll already satisfies lib.mount_is_fresh, i.e. no looping/sleeping in
    this fake.

    clock_values: one value returned per "Date.now()" evaluate call, in
    order (only one such call happens per _navigate_to_tab invocation
    today).

    live_active: what the #live-btn-state evaluate (_LIVE_ACTIVE_JS)
    reports -- True (the default) skips the live-resume branch so these
    tests stay focused on the ttfp_ms anchor itself.
    """

    def __init__(self, mount, clock_values, live_active=True, has_row=True):
        self.calls = []
        self._mount = mount
        self._clock_values = list(clock_values)
        self._live_active = live_active
        self._has_row = has_row

    def click(self, selector):
        self.calls.append(("click", selector))

    def wait_for_selector(self, selector, timeout=None):
        self.calls.append(("wait_for_selector", selector))

    def query_selector(self, selector):
        self.calls.append(("query_selector", selector))
        if "clickable" in selector:
            return _FakeRow(self, selector) if self._has_row else None
        return None

    def wait_for_timeout(self, ms):
        self.calls.append(("wait_for_timeout", ms))

    def evaluate(self, script, *args):
        if script == "Date.now()":
            self.calls.append(("evaluate", "Date.now()"))
            return self._clock_values.pop(0)
        if script == smoke.VIEW_MOUNT_JS:
            self.calls.append(("evaluate", "VIEW_MOUNT_JS"))
            return {"missing": False, "mount": dict(self._mount)}
        if script == smoke._LIVE_ACTIVE_JS:
            self.calls.append(("evaluate", "LIVE_ACTIVE_JS"))
            return self._live_active
        raise AssertionError(f"unexpected evaluate() script: {script!r}")


# ── ttfp_ms value + wiring into build_tab_result ────────────────────────────

def test_navigate_returns_ttfp_ms_as_mount_minus_nav_start():
    mount = {"id": "overview", "seq": 1, "at": 5137}
    page = _FakePage(mount, clock_values=[5000])
    ttfp_ms = smoke._navigate_to_tab(page, "overview", timeout_s=5)
    check(ttfp_ms == 137, f"ttfp_ms is mount.at - nav_start_ms (5137-5000=137), got {ttfp_ms}")


def test_ttfp_ms_reaches_build_tab_result_unaveraged():
    """The regression this guards: if a future edit stops plumbing
    _navigate_to_tab's return value into build_tab_result's ttfp_ms kwarg,
    this fails -- result["ttfp_ms"] would silently read back as the
    default (None) instead of the measured value. Also proves the field is
    the RAW per-navigation number, not a mean/rounded aggregate."""
    mount = {"id": "waterfall", "seq": 4, "at": 20345}
    page = _FakePage(mount, clock_values=[20000])
    ttfp_ms = smoke._navigate_to_tab(page, "waterfall", timeout_s=5)
    result = lib.build_tab_result(
        "waterfall", True, "ok", ticks_observed=6, console_errors=[],
        blink_ratio=0.0, color_violations=[],
        leak_before={"charts": 1, "uplots": 1, "pending": 0},
        leak_after={"charts": 1, "uplots": 1, "pending": 0},
        artifacts={}, ttfp_ms=ttfp_ms)
    check(result["ttfp_ms"] == 345,
          f"build_tab_result carries the exact measured ttfp_ms through "
          f"(20345-20000=345), got {result['ttfp_ms']!r}")


def test_ttfp_ms_defaults_to_none_when_not_measured():
    """A failed-navigation result (build_failed_tab_result, the caller has
    no ttfp_ms to give) must read back None, not 0 or a stale value from a
    previous call -- 0 would be silently indistinguishable from a
    genuinely instant (0ms) paint."""
    r = lib.build_failed_tab_result("timeline", "panel did not render within 60s")
    check(r["ttfp_ms"] is None,
          "a failed navigation's ttfp_ms is None, never a fabricated 0")


# ── Timeline: the drill-down, not the bare click ────────────────────────────

def test_navigate_nontimeline_anchors_ttfp_at_the_landing_click():
    """Every non-Timeline tab's landing click IS the bare tab click -- the
    ttfp_ms anchor (Date.now()) must be captured immediately before it."""
    mount = {"id": "sessions", "seq": 1, "at": 1200}
    page = _FakePage(mount, clock_values=[1000])
    smoke._navigate_to_tab(page, "sessions", timeout_s=5)
    date_idx = page.calls.index(("evaluate", "Date.now()"))
    click_idx = page.calls.index(("click", ".tab[data-tab='sessions']"))
    check(date_idx < click_idx,
          "nav_start_ms (Date.now()) is captured BEFORE the landing click "
          f"for a non-Timeline tab (date_idx={date_idx}, click_idx={click_idx})")


def test_navigate_timeline_spans_the_drill_down_not_the_bare_click():
    """issue #242's named trap (retracted by issue #245): an earlier 30s
    Timeline finding measured the Sessions-tab BARE CLICK's own render (the
    "select a session" prompt), not Timeline's. This asserts the ttfp_ms
    anchor is captured AFTER the Sessions tab's own table has rendered
    (page.wait_for_selector for its rows) and the clickable row has been
    located, and BEFORE the row click that actually drills into Timeline --
    so ttfp_ms measures the drill-down's own paint, never the bare click's.

    What would make this fail: a regression that captures nav_start_ms
    right after `.tab[data-tab='sessions']` is clicked (before waiting for
    the table) -- exactly the retracted bug. See this module's own
    docstring / the branch's report for a demonstrated-red run against
    that exact regression."""
    mount = {"id": "timeline", "seq": 1, "at": 9000}
    page = _FakePage(mount, clock_values=[8850])
    ttfp_ms = smoke._navigate_to_tab(page, "timeline", timeout_s=5)
    check(ttfp_ms == 150, f"ttfp_ms = 9000-8850 = 150, got {ttfp_ms}")

    sessions_click_idx = page.calls.index(("click", ".tab[data-tab='sessions']"))
    table_wait_idx = page.calls.index(("wait_for_selector",
                                       "#table-container table tbody tr"))
    row_query_idx = page.calls.index(("query_selector",
                                      "#table-container table tbody tr.clickable"))
    date_idx = page.calls.index(("evaluate", "Date.now()"))
    row_click_idx = page.calls.index(("row_click",
                                      "#table-container table tbody tr.clickable"))

    check(sessions_click_idx < table_wait_idx,
          "sanity: the bare Sessions-tab click happens before its own table wait")
    check(table_wait_idx < row_query_idx,
          "sanity: the table wait happens before the row is located")
    check(row_query_idx < date_idx < row_click_idx,
          "ttfp_ms anchor (Date.now()) is captured AFTER the Sessions bare "
          "click's own render/table-wait AND the row lookup, and BEFORE "
          "the drill-down row click -- so it spans the drill, never the "
          f"bare click (row_query_idx={row_query_idx}, date_idx={date_idx}, "
          f"row_click_idx={row_click_idx})")


def test_navigate_timeline_no_clickable_row_raises_loudly():
    mount = {"id": "timeline", "seq": 1, "at": 9000}
    page = _FakePage(mount, clock_values=[8850], has_row=False)
    try:
        smoke._navigate_to_tab(page, "timeline", timeout_s=5)
        check(False, "no clickable row must raise SmokeFailure, not silently proceed")
    except smoke.SmokeFailure:
        check(True, "no clickable row raises SmokeFailure loudly")


# ── run_tab()'s own call site ───────────────────────────────────────────────
#
# run_tab() itself (unlike _navigate_to_tab) is a full Playwright page
# lifecycle -- browser.new_context(), page.goto(), video recording, the
# live-tick loop -- that this fast unit-test tier deliberately does not
# stub (see module docstring: no browser, no I/O). Without SOME check here,
# the wiring `ttfp_ms=ttfp_ms` at run_tab's build_tab_result call site has
# NO fast-tier coverage at all -- test_ttfp_ms_reaches_build_tab_result_
# unaveraged above calls build_tab_result directly and cannot see a
# regression in run_tab's OWN call. An AST check of run_tab's source is the
# cheapest thing that actually looks at that call site instead of assuming
# it: it fails if the ttfp_ms= kwarg is dropped, renamed, or wired to
# anything other than the local variable assigned from
# _navigate_to_tab(...)'s own return value.

def test_run_tab_wires_ttfp_ms_into_build_tab_result():
    tree = ast.parse(inspect.getsource(smoke.run_tab))
    build_calls = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                   and isinstance(n.func, ast.Attribute)
                   and n.func.attr == "build_tab_result"]
    check(len(build_calls) == 1, "run_tab calls lib.build_tab_result exactly once")
    if not build_calls:
        return
    kwarg = next((kw for kw in build_calls[0].keywords if kw.arg == "ttfp_ms"), None)
    check(kwarg is not None, "run_tab's build_tab_result call passes a ttfp_ms= kwarg")
    check(kwarg is not None and isinstance(kwarg.value, ast.Name)
          and kwarg.value.id == "ttfp_ms",
          "the ttfp_ms kwarg's value is the `ttfp_ms` local variable, "
          "not a literal or a differently-named variable")

    assigns = [n for n in ast.walk(tree) if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == "ttfp_ms" for t in n.targets)
               and isinstance(n.value, ast.Call)
               and isinstance(n.value.func, ast.Name)
               and n.value.func.id == "_navigate_to_tab"]
    check(len(assigns) == 1,
          "ttfp_ms is assigned exactly once, from _navigate_to_tab(...)'s "
          "own return value -- not some other call or a hardcoded default")


def test_run_tab_except_branches_carry_ttfp_ms_into_build_failed_tab_result():
    """Companion to the AST check above, for run_tab's two `except` branches
    (SmokeFailure and bare Exception): both call build_failed_tab_result,
    and both must pass ttfp_ms=ttfp_ms too -- otherwise a value
    _navigate_to_tab already measured (navigation itself succeeded, a LATER
    tick-loop step failed) is silently dropped to None even though
    build_failed_tab_result was extended specifically to carry it through.
    `ttfp_ms` is initialised to None before the try block, so this is safe
    even when the SmokeFailure fires during navigation itself, before
    _navigate_to_tab's own return sets it."""
    tree = ast.parse(inspect.getsource(smoke.run_tab))
    failed_calls = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                    and isinstance(n.func, ast.Attribute)
                    and n.func.attr == "build_failed_tab_result"]
    check(len(failed_calls) == 2,
          f"run_tab calls lib.build_failed_tab_result exactly twice "
          f"(one per except branch), found {len(failed_calls)}")
    for i, call in enumerate(failed_calls):
        kwarg = next((kw for kw in call.keywords if kw.arg == "ttfp_ms"), None)
        check(kwarg is not None,
              f"build_failed_tab_result call #{i + 1} passes a ttfp_ms= kwarg")
        check(kwarg is not None and isinstance(kwarg.value, ast.Name)
              and kwarg.value.id == "ttfp_ms",
              f"build_failed_tab_result call #{i + 1}'s ttfp_ms kwarg is "
              "the `ttfp_ms` local variable, not a literal or a "
              "differently-named variable")

    preassigns = [n for n in ast.walk(tree) if isinstance(n, ast.Assign)
                  and any(isinstance(t, ast.Name) and t.id == "ttfp_ms"
                          for t in n.targets)
                  and isinstance(n.value, ast.Constant) and n.value.value is None]
    check(len(preassigns) == 1,
          "ttfp_ms is pre-initialised to None before the try block, so "
          "both except branches can reference it even when a SmokeFailure "
          "fires before _navigate_to_tab ever returns")


# ── the sweep anchor (this issue) ──────────────────────────────────────────
#
# THE regression this issue exists to prevent coming back. The whole tick loop
# is inside run_tab's Playwright lifecycle, which this fast tier deliberately
# does not stub, so the one-line revert that reintroduces the bug --
# `_wait_for_tick(page, i, ...)` instead of `_wait_for_tick(page,
# tick_baseline + i, ...)` -- would otherwise have NO fast-tier coverage at
# all. It would only resurface on the live tier as tick-1 frame-2 drift of
# 400-500ms, i.e. a red gate on a healthy tree: exactly the ~50% flake this
# issue removed, and exactly the symptom whose last diagnosis cost a full
# investigation. An AST check of run_tab's own source is the cheapest thing
# that looks at the call site rather than assuming it.

def test_run_tab_waits_for_a_tick_baselined_after_navigation():
    tree = ast.parse(inspect.getsource(smoke.run_tab))

    # 1. The baseline is read from the tick hook's own length, ONCE.
    baseline_assigns = [
        n for n in ast.walk(tree) if isinstance(n, ast.Assign)
        and any(isinstance(t, ast.Name) and t.id == "tick_baseline" for t in n.targets)]
    check(len(baseline_assigns) == 1,
          "run_tab assigns tick_baseline exactly once")
    if baseline_assigns:
        src = ast.dump(baseline_assigns[0])
        check("__uiLiveTicks" in src and "length" in src,
              "tick_baseline is read from window.__uiLiveTicks.length, not "
              "guessed or hardcoded")

    # 2. It is read AFTER navigation and the leak probe -- a baseline taken
    #    before either would still include their AAS sends and change nothing.
    def first_line(pred):
        return min((n.lineno for n in ast.walk(tree)
                    if isinstance(n, ast.Call) and pred(n)), default=None)

    nav_line = first_line(lambda n: isinstance(n.func, ast.Name)
                          and n.func.id == "_navigate_to_tab")
    leak_line = first_line(lambda n: isinstance(n.func, ast.Name)
                           and n.func.id == "_settled_leak_probe")
    check(nav_line is not None and leak_line is not None,
          "run_tab still calls _navigate_to_tab and _settled_leak_probe")
    if baseline_assigns and nav_line and leak_line:
        check(baseline_assigns[0].lineno > nav_line,
              "tick_baseline is read AFTER _navigate_to_tab (whose tab click "
              "and #live-btn resume click each send AAS)")
        check(baseline_assigns[0].lineno > leak_line,
              "tick_baseline is read AFTER _settled_leak_probe, so the probe's "
              "own elapsed time cannot land between the anchor and the sweep")

    # 3. _wait_for_tick's target is OFFSET BY the baseline, not a bare `i`.
    waits = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
             and isinstance(n.func, ast.Name) and n.func.id == "_wait_for_tick"]
    check(len(waits) == 1, "run_tab calls _wait_for_tick exactly once")
    if waits:
        target = waits[0].args[1] if len(waits[0].args) > 1 else None
        check(isinstance(target, ast.BinOp) and isinstance(target.op, ast.Add),
              "the tick target is an addition, not a bare loop index -- a bare "
              "`i` is the exact pre-fix form: it is satisfied by navigation's "
              "own AAS sends and anchors tick 1 on a mount ~500ms stale")
        names = {n.id for n in ast.walk(target) if isinstance(n, ast.Name)} \
            if target is not None else set()
        check({"tick_baseline", "i"} <= names,
              f"the tick target adds tick_baseline to the loop index ({names})")


def test_run_tab_reads_the_tick_it_waited_for_by_index_not_last():
    """The companion half: having waited for `tick_baseline + i` ticks, the
    loop must read THAT entry, not `__uiLiveTicks[length - 1]`.

    They differ whenever another tick lands while this iteration is still
    working (its sweep alone spans ~2s of a 5s cadence), and reading `last`
    there silently re-anchors the iteration on a newer send than the one it
    represents -- the same class of bug as the stale anchor, in the opposite
    direction, and just as invisible downstream because every later number is
    derived from this timestamp."""
    src = inspect.getsource(smoke.run_tab)
    check("window.__uiLiveTicks[window.__uiLiveTicks.length - 1]" not in src,
          "run_tab no longer reads the LAST tick regardless of which one it "
          "waited for (the pre-fix form)")
    tree = ast.parse(src)
    idx_calls = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Attribute)
                 and n.func.attr == "tick_hook_index"]
    check(len(idx_calls) >= 1,
          "run_tab uses lib.tick_hook_index(...) to address the tick it "
          "waited for -- the pure, unit-tested index arithmetic that refuses "
          "a baseline/i it cannot address instead of clamping")


def _discover_tests():
    found = [obj for name, obj in list(globals().items())
             if name.startswith("test_") and inspect.isfunction(obj)]
    found.sort(key=lambda fn: inspect.getsourcelines(fn)[1])
    return found


TESTS = _discover_tests()


def main():
    print(f"discovered {len(TESTS)} test functions")
    for t in TESTS:
        print(f"--- {t.__name__} ---")
        t()
    print()
    print(f"Ran {tests_run}, passed {tests_passed}, failed {tests_failed} "
          f"({len(TESTS)} test functions discovered)")
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
