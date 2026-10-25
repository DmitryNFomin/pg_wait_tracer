#!/usr/bin/env python3
"""Bypass suite for tests/diag_537_waterfall.py's refusal logic.

The true-positive path of a reproduction sweep (it CAN report failures) was
never the risk. The risk is the FALSE NEGATIVE: a sweep that never looked,
reporting "0 of 10 -- does not reproduce", which in #537's case would demote a
release-fatal blank tab out of the blocking set on evidence that does not
exist. So there is one test per way this sweep's detection can be made
unreachable, and each asserts VOID/refusal rather than a verdict.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diag_537_waterfall import (          # noqa: E402
    DiagRefusal, classify_attempt, fail_counts, sweep_verdict, PATHS,
)


def attempt(path="hash", index=0, *, canvas=(1600, 300), rows=5,
            connected=True, alive=True, harness_error=None, snapshot=True):
    snap = None
    if snapshot:
        snap = {"execution_rows": rows,
                "canvas": None if canvas is None else
                          {"client_width": canvas[0], "client_height": canvas[1]}}
    return {"path": path, "index": index, "connected": connected,
            "bridge_alive_after": alive, "harness_error": harness_error,
            "snapshot": snap}


def sweep(fails_hash=0, fails_click=0, n=10, **kw):
    ok = kw.pop("canvas", (1600, 300))
    out = []
    for i in range(n):
        out.append(attempt("hash", i, canvas=None if i < fails_hash else ok, **kw))
        out.append(attempt("click", i, canvas=None if i < fails_click else ok, **kw))
    return out


class TruePositive(unittest.TestCase):
    """The detection works at all -- the cheap half."""

    def test_a_canvas_that_never_appears_is_a_failure(self):
        self.assertEqual(classify_attempt(attempt(canvas=None)), "fail")

    def test_three_of_ten_on_either_path_reproduces(self):
        v, _, counts = sweep_verdict(sweep(fails_hash=3), 10)
        self.assertEqual(v, "REPRODUCED")
        self.assertEqual(counts["hash"]["fail"], 3)
        v, _, _ = sweep_verdict(sweep(fails_click=3), 10)
        self.assertEqual(v, "REPRODUCED")

    def test_a_clean_sweep_reports_not_reproduced(self):
        v, _, _ = sweep_verdict(sweep(), 10)
        self.assertEqual(v, "NOT_REPRODUCED")

    def test_below_the_bar_is_intermittent_not_clean(self):
        v, reason, _ = sweep_verdict(sweep(fails_hash=2), 10)
        self.assertEqual(v, "INTERMITTENT")
        self.assertIn("below", reason)


class CannotSeeMustRefuse(unittest.TestCase):
    """Each of these made the sweep blind; each must be VOID, not a verdict."""

    def test_empty_input_is_void_not_clean(self):
        # The headline false negative: no attempts at all reading as "0/10".
        v, reason, _ = sweep_verdict([], 10)
        self.assertEqual(v, "VOID")
        self.assertIn("0 of 10", reason)

    def test_partial_range_is_void(self):
        # 10 hash attempts but only 7 click attempts: the click number does
        # not exist, so no verdict may be drawn from it.
        part = sweep(n=10)
        part = [a for a in part if not (a["path"] == "click" and a["index"] >= 7)]
        v, reason, _ = sweep_verdict(part, 10)
        self.assertEqual(v, "VOID")
        self.assertIn("click", reason)

    def test_a_page_that_never_connected_is_void_not_a_failure(self):
        # Covers this script's OWN WebSocket instrumentation breaking the app:
        # no canvas, but also no connection -- harness fault, not a defect.
        self.assertEqual(classify_attempt(
            attempt(connected=False, canvas=None)), "void")
        v, _, _ = sweep_verdict(sweep(connected=False, fails_hash=10,
                                      fails_click=10), 10)
        self.assertEqual(v, "VOID")

    def test_a_bridge_that_died_mid_sweep_is_void_not_ten_of_ten(self):
        half = sweep(n=10)
        for a in half:
            if a["index"] >= 5:
                a["bridge_alive_after"] = False
                a["snapshot"]["canvas"] = None
        v, reason, _ = sweep_verdict(half, 10)
        self.assertEqual(v, "VOID")
        self.assertIn("void", reason)

    def test_an_empty_capture_is_void_even_when_every_canvas_appeared(self):
        # The pass-while-broken input from the pre-registered Q1: zero
        # executions everywhere. Canvases "appear", so a naive sweep would
        # report a clean 0/10 against a product that renders nothing real.
        v, reason, _ = sweep_verdict(sweep(rows=0), 10)
        self.assertEqual(v, "VOID")
        self.assertIn("execution row", reason)

    def test_an_absent_snapshot_is_void_not_a_pass(self):
        self.assertEqual(classify_attempt(attempt(snapshot=False)), "void")

    def test_an_unparseable_canvas_field_is_void_not_a_pass(self):
        self.assertEqual(classify_attempt(
            {"path": "hash", "connected": True, "bridge_alive_after": True,
             "snapshot": {"execution_rows": 3, "canvas": "yes"}}), "void")
        self.assertEqual(classify_attempt(
            {"path": "hash", "connected": True, "bridge_alive_after": True,
             "snapshot": {"execution_rows": 3,
                          "canvas": {"client_width": None,
                                     "client_height": 300}}}), "void")

    def test_a_harness_error_is_void_not_a_failure(self):
        self.assertEqual(classify_attempt(
            attempt(harness_error="overview table never rendered a row",
                    canvas=None)), "void")

    def test_a_dependency_that_cannot_run_refuses_rather_than_scoring(self):
        # 126/127 and a missing playwright all arrive here as "no attempts
        # were produced"; the one thing that must never happen is a verdict.
        for bad in (None, 42, "attempt"):
            with self.assertRaises(DiagRefusal):
                fail_counts([bad])
        with self.assertRaises(DiagRefusal):
            fail_counts([{"path": "typo", "connected": True,
                          "bridge_alive_after": True, "snapshot": {}}])
        with self.assertRaises(DiagRefusal):
            sweep_verdict([], 0)

    def test_extra_attempts_cannot_pay_for_void_ones(self):
        # Same gap as the fixture path's: 12 attempts per path, 2 of them
        # blind, still leaves 10 completed and would satisfy a count-only
        # guard. The void short-circuit is what refuses it.
        attempts = sweep(n=12)
        for a in attempts:
            if a["index"] in (0, 1):
                a["connected"] = False
        v, reason, _ = sweep_verdict(attempts, 10)
        self.assertEqual(v, "VOID")
        self.assertIn("void", reason)

    def test_the_thing_checked_absent_vs_wrong_are_both_failures(self):
        # absent: no canvas element at all
        self.assertEqual(classify_attempt(attempt(canvas=None)), "fail")
        # present but zero-area -- a blank tab a visibility wait also rejects
        self.assertEqual(classify_attempt(attempt(canvas=(0, 300))), "fail")
        self.assertEqual(classify_attempt(attempt(canvas=(1600, 0))), "fail")

    def test_void_outranks_a_reproduction_too(self):
        # A sweep must not claim a reproduction it could not see either: one
        # void attempt invalidates the number in BOTH directions.
        mixed = sweep(fails_hash=9)
        mixed[0]["connected"] = False
        v, _, _ = sweep_verdict(mixed, 10)
        self.assertEqual(v, "VOID")


class Ordering(unittest.TestCase):
    def test_both_paths_are_measured(self):
        self.assertEqual(PATHS, ("hash", "click"))

    def test_void_is_checked_before_canvas_state(self):
        # A void attempt that ALSO has a perfectly good canvas is still void:
        # the guard cannot be satisfied by the happy-path field.
        a = attempt(canvas=(1600, 300), connected=False)
        self.assertEqual(classify_attempt(a), "void")




# ── part 2: the fixture path (tests/diag_537_fixture_path.py) ────────────────

from diag_537_fixture_path import (          # noqa: E402
    classify_fixture_attempt, fixture_counts, fixture_verdict,
)

CV = {"client_width": 1650, "client_height": 217}


def fx(index=0, *, before=CV, after=CV, installed=True, connected=True,
       alive=True, harness_error=None):
    return {"index": index, "connected": connected, "bridge_alive_after": alive,
            "harness_error": harness_error, "fixture_installed": installed,
            "canvas_before": before, "canvas_after": after}


class FixturePathTruePositive(unittest.TestCase):
    def test_a_canvas_that_disappears_is_detected(self):
        self.assertEqual(classify_fixture_attempt(fx(after=None)), "canvas_lost")
        v, _, counts = fixture_verdict([fx(i, after=None) for i in range(10)], 10)
        self.assertEqual(v, "FIXTURE_REMOVES_CANVAS")
        self.assertEqual(counts["canvas_lost"], 10)

    def test_a_surviving_canvas_reads_harmless(self):
        v, _, _ = fixture_verdict([fx(i) for i in range(10)], 10)
        self.assertEqual(v, "FIXTURE_HARMLESS")

    def test_even_one_loss_is_reported(self):
        attempts = [fx(i) for i in range(10)]
        attempts[4]["canvas_after"] = None
        v, reason, _ = fixture_verdict(attempts, 10)
        self.assertEqual(v, "FIXTURE_REMOVES_CANVAS")
        self.assertIn("1/10", reason)


class FixturePathCannotSeeMustRefuse(unittest.TestCase):
    def test_no_canvas_before_the_fixture_proves_nothing(self):
        # The headline false POSITIVE for this script: an attempt that never
        # had a canvas would otherwise be scored as the fixture removing one.
        self.assertEqual(classify_fixture_attempt(fx(before=None, after=None)),
                         "void")
        v, _, _ = fixture_verdict(
            [fx(i, before=None, after=None) for i in range(10)], 10)
        self.assertEqual(v, "VOID")

    def test_a_fixture_that_never_installed_is_void_not_harmless(self):
        # transport.request unwrapped, the sort toggle missing, the response
        # empty: the fixture did not run, so "canvas survived" is vacuous.
        self.assertEqual(classify_fixture_attempt(fx(installed=False)), "void")
        v, _, _ = fixture_verdict([fx(i, installed=False) for i in range(10)], 10)
        self.assertEqual(v, "VOID")

    def test_empty_and_partial_sweeps_are_void(self):
        self.assertEqual(fixture_verdict([], 10)[0], "VOID")
        self.assertEqual(fixture_verdict([fx(i) for i in range(6)], 10)[0], "VOID")

    def test_a_dead_bridge_or_harness_error_is_void(self):
        self.assertEqual(classify_fixture_attempt(fx(alive=False, after=None)),
                         "void")
        self.assertEqual(classify_fixture_attempt(
            fx(harness_error="boom", after=None)), "void")
        self.assertEqual(classify_fixture_attempt(fx(connected=False)), "void")

    def test_a_zero_area_canvas_counts_as_absent_on_both_sides(self):
        self.assertEqual(classify_fixture_attempt(
            fx(after={"client_width": 0, "client_height": 217})), "canvas_lost")
        self.assertEqual(classify_fixture_attempt(
            fx(before={"client_width": 0, "client_height": 217})), "void")

    def test_extra_attempts_cannot_pay_for_void_ones(self):
        # The gap the mutation suite found (F3 was initially not
        # load-bearing): with MORE attempts than requested, the partial-sweep
        # guard is satisfied by the completed ones and a sweep containing
        # blind attempts would be graded anyway. Retried/extra attempts must
        # not launder a void.
        attempts = [fx(i) for i in range(12)]
        attempts[0]["connected"] = False
        attempts[1]["canvas_before"] = None
        v, reason, _ = fixture_verdict(attempts, 10)
        self.assertEqual(v, "VOID")
        self.assertIn("void", reason)

    def test_an_unparseable_record_refuses(self):
        for bad in (None, 7, "rec"):
            with self.assertRaises(DiagRefusal):
                fixture_counts([bad])
        with self.assertRaises(DiagRefusal):
            fixture_verdict([fx(0)], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
