#!/usr/bin/env python3
"""Unit tests for tests/timeline_window_probe.py's pure parts (issue #205).

The probe's value is entirely in what it REFUSES. Its true-positive path is
easy and was demonstrated live (a frozen window against tests/mock_server.py,
whose `now_ns` is a pinned constant, reads 7 frozen pairs of 7; the real
server on the gate box reads 0 of 30). What is worth pinning here is the
other direction: every way the probe can be starved of the thing it measures
must come out as a refusal, never as a clean zero that a reader would take
for "the window is fine" -- or, just as bad, for "the window is frozen" when
in truth nothing was observed at all.

No Playwright, no browser, no network: these are the two pure functions.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

failures = []
checks = 0


def check(name, cond, detail=""):
    global checks
    checks += 1
    if cond:
        print("  PASS: %s" % name)
    else:
        print("  FAIL: %s %s" % (name, detail))
        failures.append(name)


def refuses(name, fn, expect_substr):
    """The callable must raise ProbeFailure, and its message must name the
    reason -- a refusal whose text does not say what was missing sends the
    reader looking in the wrong place."""
    global checks
    checks += 1
    try:
        fn()
    except probe.ProbeFailure as e:
        if expect_substr in str(e):
            print("  PASS: %s  [refused: %s]" % (name, e))
            return
        print("  FAIL: %s -- refused, but not about %r: %s"
              % (name, expect_substr, e))
        failures.append(name)
        return
    except Exception as e:                        # noqa: BLE001
        print("  FAIL: %s -- raised %s, not a ProbeFailure: %s"
              % (name, type(e).__name__, e))
        failures.append(name)
        return
    print("  FAIL: %s -- did NOT refuse; it approved a run it could not see"
          % name)
    failures.append(name)


def frame(cmd, to="1000"):
    return {"at": 1, "id": 1, "cmd": cmd, "from": "0", "to": to}


def main():
    print("== select_measurable: the blindness guard ==")

    # The thing being checked is ABSENT, not wrong. Nothing was recorded at
    # all, so there is no evidence either way -- the one case a gate is most
    # likely to score as a pass.
    refuses("no frames at all -> refuses",
            lambda: probe.select_measurable([], 6), "never ticked")
    refuses("hook never installed (None, not a list) -> refuses",
            lambda: probe.select_measurable(None, 6), "never installed")

    # The live loop ticks, but the tab under test never fetched: `info` alone
    # would satisfy a naive "did anything happen" check.
    refuses("info ticking but zero session_timeline -> refuses",
            lambda: probe.select_measurable([frame("info")] * 8, 6),
            "session_timeline")

    # A PARTIAL run: the tab fetched, just not for as many ticks as were
    # asked for. Summarising 2 of 6 would be a verdict on a quarter of the
    # evidence.
    refuses("partial run (2 of 6 ticks) -> refuses",
            lambda: probe.select_measurable(
                [frame("info")] * 6 + [frame("session_timeline")] * 2, 6),
            "only 2")

    # The tab fetched but the live loop is not running: every window would be
    # identical for a reason that is not the product's freshness.
    refuses("session_timeline without any info -> refuses",
            lambda: probe.select_measurable([frame("session_timeline")] * 8, 6),
            "never ticked")

    # An UNPARSEABLE field. Silently skipping these would quietly shrink the
    # sample the verdict is computed over.
    refuses("a frame with no readable cmd -> refuses",
            lambda: probe.select_measurable(
                [frame("info")] * 6 + [frame("session_timeline")] * 6
                + [{"at": 1, "id": 2}], 6),
            "readable `cmd`")
    refuses("a frame whose cmd is not a string -> refuses",
            lambda: probe.select_measurable(
                [frame("info")] * 6 + [frame("session_timeline")] * 6
                + [{"cmd": 17}], 6),
            "readable `cmd`")

    # ...and the one shape that IS measurable must pass, or the guard is just
    # a refusal machine that can never say yes.
    sent = [frame("info")] * 6 + [frame("session_timeline")] * 6 + [frame("aas")] * 6
    tl, info = probe.select_measurable(sent, 6)
    check("a complete run is accepted", len(tl) == 6 and len(info) == 6,
          "(got %d timeline, %d info)" % (len(tl), len(info)))

    print("== frozen_pair_count: the measurement itself ==")
    # #205's world: the window never moved across six ticks.
    check("a frozen window reads 5 frozen pairs of 5",
          probe.frozen_pair_count(["100"] * 6) == 5,
          "(got %d)" % probe.frozen_pair_count(["100"] * 6))
    # The gate box's measured world: 5 s per tick.
    moving = [str(100 + 5 * i) for i in range(6)]
    check("a 5-per-tick window reads 0 frozen pairs",
          probe.frozen_pair_count(moving) == 0,
          "(got %d)" % probe.frozen_pair_count(moving))
    # The reported shape: four identical then two advances. If this read 0,
    # the probe could not have distinguished #205's report from a healthy run.
    mixed = ["100", "100", "100", "100", "105", "110"]
    check("four-identical-then-two-advances reads 3 frozen pairs",
          probe.frozen_pair_count(mixed) == 3,
          "(got %d)" % probe.frozen_pair_count(mixed))
    # A single sample cannot produce a pair, and must not be counted as one.
    check("a single sample reads 0 pairs (never a fabricated verdict)",
          probe.frozen_pair_count(["100"]) == 0)

    print("\n%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        import timeline_window_probe as probe
    except ImportError as e:
        # The module imports Playwright at top level. On a host without it the
        # right answer is a loud skip-as-failure, never a silent pass: a test
        # that cannot import what it tests has not tested anything.
        print("SKIP-AS-FAIL: cannot import timeline_window_probe: %s" % e)
        sys.exit(1)
    sys.exit(main())
