#!/usr/bin/env python3
"""test_ui_live_smoke_inject.py -- issue #266: unit tests for
tests/ui_live_smoke.py's `_capture_sweep_offset`, the function the #252
self-test's injected-overlay bracket now lives in.

Mechanism under test: the overlay used to be shown/hidden by a browser-side
setTimeout scheduled against an absolute time guessed BEFORE the sweep ever
started (mount_at_ms + 700 / +1100) -- a bet that the targeted sample's
capture would land inside that fixed window. Measured on the gate box: the
achieved offset alone (1008-1014ms) left ~87ms of margin against a
screenshot call that itself costs 210-344ms, so the real captured instant
could land past the window's close -- about 1 run in 3 on identical code
(issue #266). `_capture_sweep_offset` replaces the guess: it shows the
overlay synchronously immediately BEFORE the capture call it targets, and
hides it synchronously AFTER that call returns, held open for at least the
configured `blink_ms` by sleeping any remainder first. This is the
invariant these tests check: show happens no later than the capture starts,
and hide happens no earlier than the capture finishes -- true by
construction, never by a timing guess, so it cannot race regardless of how
long any single capture call takes.

Needs Playwright importable, same reason as test_ui_live_smoke_nav.py (see
that file's own docstring) -- not in tests/unit_tests.list, runs instead as
its own step in scripts/check.sh's full tier.

No browser, no I/O: `_capture_sweep_offset` is exercised against a
hand-written fake Page object with its own virtual millisecond clock (Date.
now() reads it, wait_for_timeout advances it, screenshot() advances it by a
configurable "capture latency" -- the exact 210-344ms issue #266 measured),
so this stays a fast, deterministic unit test, same idiom as
tests/test_ui_live_smoke_nav.py.

Usage: python3 tests/test_ui_live_smoke_inject.py
"""
import inspect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_live_smoke as smoke

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


class _FakePage:
    """A virtual millisecond clock standing in for the browser's Date.now():
    evaluate("Date.now()") reads it, wait_for_timeout(ms) advances it by ms
    (exactly like the real browser clock advancing while Python sleeps),
    and screenshot(...) advances it by `capture_advance_ms` -- standing in
    for the real wall-clock cost a screenshot call has (issue #266's
    210-344ms, mostly Playwright's own stability wait) that the real
    browser clock keeps ticking through. Every call is logged, WITH the
    clock reading at the time of the call, in self.calls -- the whole point
    is to let a test assert WHEN show/hide land relative to the capture,
    not just that they were called at all."""

    def __init__(self, start_ms, capture_advance_ms):
        self.calls = []
        self.now_ms = start_ms
        self.capture_advance_ms = capture_advance_ms

    def evaluate(self, script, arg=None):
        if script == "Date.now()":
            self.calls.append(("evaluate_now", self.now_ms))
            return self.now_ms
        if script == smoke._SHOW_BLINK_OVERLAY_JS:
            self.calls.append(("show", self.now_ms, dict(arg)))
            return None
        if script == smoke._HIDE_BLINK_OVERLAY_JS:
            self.calls.append(("hide", self.now_ms))
            return None
        raise AssertionError(f"unexpected evaluate() script: {script!r}")

    def wait_for_timeout(self, ms):
        self.calls.append(("wait_for_timeout", self.now_ms, ms))
        self.now_ms += ms

    def screenshot(self, clip=None, scale=None):
        start = self.now_ms
        self.calls.append(("screenshot_start", start))
        self.now_ms += self.capture_advance_ms
        self.calls.append(("screenshot_end", self.now_ms))
        return b"FAKE-PNG-BYTES"


def _call_times(calls, name):
    return [c[1] for c in calls if c[0] == name]


# ── no injection: behaviour is unchanged from plain _capture_at_offset ──────

def test_no_inject_shows_nothing():
    page = _FakePage(start_ms=990, capture_advance_ms=250)
    frame, achieved, capture_ms = smoke._capture_sweep_offset(
        page, "sessions", 0, 1000, clip={"x": 0, "y": 0, "w": 1}, inject=None)
    check(frame == b"FAKE-PNG-BYTES", "frame is the screenshot's own bytes")
    check(achieved == 1000, f"achieved offset is the target (1000), got {achieved}")
    check(not any(c[0] in ("show", "hide") for c in page.calls),
          "no inject given -> no show/hide calls at all")


# ── injection brackets the real capture, never a scheduled guess ───────────

def test_inject_show_before_capture_hide_after_capture_fast_capture():
    """issue #266's own measured numbers: achieved 1000ms, a 300ms capture
    (inside the observed 210-344ms range) -- the overlay must be visible
    for the capture's ENTIRE span, bracketing it on both sides."""
    page = _FakePage(start_ms=990, capture_advance_ms=300)
    inject = {"blink_ms": 400, "x": 1, "y": 2, "width": 3, "height": 4}
    frame, achieved, capture_ms = smoke._capture_sweep_offset(
        page, "sessions", 0, 1000, clip={"x": 0, "y": 0}, inject=inject)

    show_ms = _call_times(page.calls, "show")[0]
    cap_start_ms = _call_times(page.calls, "screenshot_start")[0]
    cap_end_ms = _call_times(page.calls, "screenshot_end")[0]
    hide_ms = _call_times(page.calls, "hide")[0]

    check(achieved == 1000, f"achieved offset unaffected by injection, got {achieved}")
    check(show_ms <= cap_start_ms,
          f"overlay shown AT OR BEFORE the capture starts (show={show_ms}, "
          f"capture_start={cap_start_ms}) -- never after")
    check(hide_ms >= cap_end_ms,
          f"overlay hidden AT OR AFTER the capture finishes (hide={hide_ms}, "
          f"capture_end={cap_end_ms}) -- this is the #266 fix: the OLD "
          f"mechanism hid at a fixed absolute time computed before the "
          f"sweep started, independent of how long THIS capture actually "
          f"took, and could hide before a slow capture finished")
    check(page.calls[0][0] == "evaluate_now" and page.calls[1][0] == "show",
          f"the show bracket (read clock, then show) is the very first "
          f"thing done, before any capture work starts, got "
          f"{[c[0] for c in page.calls[:2]]}")
    check(page.calls[-1][0] == "hide", "hide is the very last call made")


def test_inject_hide_waits_for_a_slow_capture_past_blink_ms():
    """The regression this guards against directly: a capture SLOWER than
    the configured blink_ms (here 500ms capture vs 400ms blink_ms) must
    still only be hidden AFTER the capture returns -- never at show_ms +
    blink_ms, which would land mid-capture and is exactly how issue #266's
    fixed window (hideAtMs = showAtMs + inject_blink_ms, independent of the
    real capture) raced about 1 run in 3."""
    page = _FakePage(start_ms=1000, capture_advance_ms=500)
    inject = {"blink_ms": 400, "x": 0, "y": 0, "width": 10, "height": 2}
    smoke._capture_sweep_offset(page, "sessions", 0, 1000,
                                 clip={"x": 0, "y": 0}, inject=inject)
    show_ms = _call_times(page.calls, "show")[0]
    cap_end_ms = _call_times(page.calls, "screenshot_end")[0]
    hide_ms = _call_times(page.calls, "hide")[0]
    naive_hide_ms = show_ms + inject["blink_ms"]
    check(cap_end_ms > naive_hide_ms,
          f"sanity: this capture (ends {cap_end_ms}) genuinely outlasts "
          f"show_ms+blink_ms ({naive_hide_ms}) -- otherwise this test "
          f"proves nothing")
    check(hide_ms >= cap_end_ms,
          f"hide ({hide_ms}) still waits for the real capture to finish "
          f"({cap_end_ms}), NOT the naive show+blink_ms deadline "
          f"({naive_hide_ms}) that a timer-based hide would have used")


def test_inject_only_fires_for_the_offset_it_is_given_for():
    """run_tab only builds an `inject` dict for
    INJECT_BLINK_TARGET_OFFSET_MS; passing inject=None for every other
    sweep offset (its own job, not this function's) must produce the exact
    same call sequence as the no-injection case."""
    page = _FakePage(start_ms=200, capture_advance_ms=50)
    smoke._capture_sweep_offset(page, "sessions", 0, 200,
                                 clip={"x": 0, "y": 0}, inject=None)
    check(not any(c[0] in ("show", "hide") for c in page.calls),
          "an untargeted offset (inject=None) never shows/hides the overlay")


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
