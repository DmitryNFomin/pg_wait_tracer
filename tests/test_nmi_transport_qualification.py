#!/usr/bin/env python3
"""Portable gate tests using hand-counted synthetic bench reports. Refs #871."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("transport_qualification", Path(__file__).parent / "bench_nmi_transport/qualify.py")
qual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qual)


def report():
    # One CPU: DATA attempts 1..3; two boundaries, all delivered in order.
    return {
        "accepted": True, "stage": "complete", "libbpf_error": 0,
        "bad_records": 0, "accounting_ok": True, "seq_reorders": 0,
        "seal_violations": 0, "timestamp_regressions": 0, "lost_notifications": 0,
        "attempts": 5, "busy_cpu_ns": 50, "elapsed": 1, "hits": 3, "migrations": 0,
        "pause_drain": False, "buffer_bytes": [67108864], "boundary_offset_ns": 0,
        "per_cpu": [{"cpu": 0, "max_boundary_gap_ns": 100_000_000,
                     "max_boundary_stamp_gap_ns": 10_000_000, "sites": [
                         {"site": "DATA", "produced": 3, "successes": 3, "failures": 0,
                          "delivered": 3, "loss": 0, "seq_gaps": 0, "reconciled": True},
                         {"site": "BOUNDARY", "produced": 2, "successes": 2, "failures": 0,
                          "delivered": 2, "loss": 0, "seq_gaps": 0, "reconciled": True}]}]
    }


def quartet(case="all"):
    runs = {arm: report() for arm in qual.ARMS}
    for arm, run in runs.items():
        if arm in ("shared", "perf"):
            run["per_cpu"][0]["sites"][1].update(produced=0, successes=0, delivered=0)
        if case == "idle":
            run["hits"] = 0
            run["per_cpu"][0]["sites"][0].update(produced=0, successes=0, delivered=0)
        run["attempts"] = sum(s["produced"] for _, s in qual.sites(run))
    return runs


class Gates(unittest.TestCase):
    def test_shared_loss_is_a_measurement_perf_loss_fails_cell(self):
        runs = quartet()
        runs["shared"]["per_cpu"][0]["sites"][0].update(
            successes=2, failures=1, delivered=2, loss=1, seq_gaps=1)
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        # Even shared reconciliation/LOST diagnostics cannot become a loss gate.
        runs["shared"].update(accounting_ok=False, lost_notifications=1)
        runs["shared"]["per_cpu"][0]["sites"][0].update(reconciled=False, seq_gaps=0)
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        for arm in ("perf", "perf_boundary"):
            with self.subTest(arm=arm):
                broken = copy.deepcopy(runs)
                broken[arm]["per_cpu"][0]["sites"][0].update(delivered=2, loss=1, seq_gaps=1)
                result = qual.evaluate_runs(broken, 1, "all", True)
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(any(arm + ": CPU 0 DATA: nonzero loss" == e for e in result["errors"]))
                broken = copy.deepcopy(runs)
                broken[arm]["per_cpu"][0]["sites"][0]["reconciled"] = False
                self.assertTrue(any("loss/sequence mismatch" in e for e in
                                    qual.evaluate_runs(broken, 1, "all", True)["errors"]))
        runs["shared"]["bad_records"] = 1
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")

    def test_irq_accounting_absent_is_unmeasured_and_void(self):
        runs = quartet("idle")
        for config in (b"# CONFIG_IRQ_TIME_ACCOUNTING is not set\n", b"CONFIG_IRQ_TIME_ACCOUNTING=n\n", b""):
            with self.subTest(config=config):
                self.assertFalse(qual.irq_time_accounting(config))
                result = qual.evaluate_runs(runs, 1, "idle", qual.irq_time_accounting(config))
                self.assertEqual(result["boundary_cpu_gate"]["status"], "UNMEASURED")
                self.assertIn("CONFIG_IRQ_TIME_ACCOUNTING", result["boundary_cpu_gate"]["reason"])
                self.assertEqual(result["status"], "VOID")
        enabled = qual.irq_time_accounting(b"CONFIG_IRQ_TIME_ACCOUNTING=y\n")
        self.assertTrue(enabled)
        result = qual.evaluate_runs(runs, 1, "idle", enabled)
        self.assertEqual(result["boundary_cpu_gate"]["status"], "PASS")
        self.assertEqual(result["status"], "PASS")
        # IRQ accounting is required for the idle boundary-cost gate only.
        self.assertEqual(qual.evaluate_runs(quartet(), 1, "all", False)["status"], "PASS")

    def test_adversarial_cpu_is_measured_but_only_normal_boundary_is_cpu_gated(self):
        runs = quartet("idle")
        runs["adversarial"]["busy_cpu_ns"] += 500_000_000
        result = qual.evaluate_runs(runs, 1, "idle", True)
        self.assertEqual(result["status"], "PASS")
        self.assertGreater(result["boundary_increment_core_fraction_per_cpu"]["adversarial"], 0.01)
        runs["perf_boundary"]["busy_cpu_ns"] += 10_000_000
        self.assertEqual(qual.evaluate_runs(runs, 1, "idle", True)["status"], "PASS")
        runs["perf_boundary"]["busy_cpu_ns"] += 1
        result = qual.evaluate_runs(runs, 1, "idle", True)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["boundary_cpu_gate"]["status"], "FAIL")
        self.assertTrue(any("perf_boundary: boundary-only" in e for e in result["errors"]))
        for field in ("seal_violations", "timestamp_regressions"):
            broken = quartet("idle")
            broken["adversarial"][field] = 1
            self.assertEqual(qual.evaluate_runs(broken, 1, "idle", True)["status"], "FAIL", field)

    def test_adversarial_loss_and_deadline_are_diagnostics_seals_still_fail(self):
        runs = quartet()
        adversarial = runs["adversarial"]
        adversarial.update(accounting_ok=False, lost_notifications=1)
        adversarial["per_cpu"][0]["sites"][0].update(
            delivered=2, loss=1, seq_gaps=0, reconciled=False)
        adversarial["per_cpu"][0]["max_boundary_gap_ns"] += 1
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        adversarial["per_cpu"][0]["seal_violations"] = 1
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")

    def test_boundary_deadline_equality_and_one_ns_over(self):
        r = report()
        self.assertEqual(qual.normal_errors(r, 1, "all", True), [])
        r["per_cpu"][0]["max_boundary_gap_ns"] += 1
        self.assertTrue(any("100 ms" in e for e in qual.normal_errors(r, 1, "all", True)))

    def test_each_cpu_and_site_loss_is_checked(self):
        for site in (0, 1):
            r = report()
            s = r["per_cpu"][0]["sites"][site]
            # v4.18 can return helper success even when perf drops a sample.
            s.update(delivered=s["delivered"] - 1, loss=1, seq_gaps=1)
            self.assertTrue(any("nonzero loss" in e for e in qual.normal_errors(r, 1, "all", True)))
            s["seq_gaps"] = 0
            self.assertTrue(any("loss/sequence mismatch" in e for e in qual.normal_errors(r, 1, "all", True)))

    def test_silent_boundary_cpu_and_missing_writer_are_void(self):
        r = report()
        r["per_cpu"][0]["sites"][1].update(produced=0, successes=0, delivered=0)
        self.assertTrue(any("boundary did not fire" in e for e in qual.base_errors(r, 1, "all", True)))
        r = report()
        r["per_cpu"][0]["sites"][0].update(produced=0, successes=0, delivered=0)
        self.assertTrue(any("active writer did not fire" in e for e in qual.base_errors(r, 1, "all", True)))

    def test_lost_is_independent_and_seals_are_not_ignored(self):
        for field in ("lost_notifications", "seal_violations", "timestamp_regressions", "bad_records"):
            r = report()
            r[field] = 1
            self.assertTrue(qual.normal_errors(r, 1, "all", True), field)

    def test_controls_must_reach_intended_assertions(self):
        loss, mutant = report(), report()
        loss["pause_drain"] = True
        loss["buffer_bytes"] = [qual.os.sysconf("SC_PAGESIZE")]
        loss["per_cpu"][0]["sites"][1].update(produced=0, successes=0, delivered=0)
        # One trailing DATA lost, but helper successes=attempts and LOST=0:
        # the pre-5.3 helper's silent drop must still fire the control.
        loss["per_cpu"][0]["sites"][0].update(delivered=2, loss=1, seq_gaps=1)
        mutant["boundary_offset_ns"] = 1_000_000
        mutant["seal_violations"] = 1
        self.assertEqual(qual.control_errors(loss, mutant, 1), [])
        loss["per_cpu"][0]["sites"][0].update(delivered=3, loss=0, seq_gaps=0)
        self.assertTrue(any("loss control did not fire" in e for e in qual.control_errors(loss, mutant, 1)))
        mutant["seal_violations"] = 0
        self.assertTrue(any("boundary control did not fire" in e for e in qual.control_errors(loss, mutant, 1)))

    def test_cpu_cost_uses_attempts_and_idle_box_delta(self):
        shared, perf = report(), report()
        perf.update(busy_cpu_ns=55)
        self.assertAlmostEqual(qual.cpu_cost_ratio(perf, shared), 1.10)
        perf["attempts"] = 10
        self.assertAlmostEqual(qual.cpu_cost_ratio(perf, shared), 0.55)
        perf["attempts"] = 0
        self.assertIsNone(qual.cpu_cost_ratio(perf, shared))
        baseline, boundary = report(), report()
        baseline.update(busy_cpu_ns=200_000_000, elapsed=2)
        boundary.update(busy_cpu_ns=240_000_000, elapsed=2)
        self.assertAlmostEqual(qual.idle_increment(boundary, baseline, 2), 0.01)
        boundary["busy_cpu_ns"] += 4
        self.assertGreater(qual.idle_increment(boundary, baseline, 2), 0.01)

    def test_wrong_treatment_is_void(self):
        r = report()
        r.update(transport="perf", case="all", cpus=1, duration=30,
                 period_ns=100_000, buffer_bytes_total=67108864)
        args = ["--transport", "perf", "--case", "all", "--cpus", "1", "--duration", "30",
                "--period-ns", "100000", "--bytes", "67108864"]
        self.assertEqual(qual.requested_errors(r, args, [0]), [])
        r["period_ns"] = 10_000_000
        self.assertTrue(any("period_ns" in e for e in qual.requested_errors(r, args, [0])))
        r["period_ns"] = 100_000
        self.assertTrue(any("CPU identities" in e for e in qual.requested_errors(r, args, [1])))
        r["buffer_bytes"] = [4096]
        self.assertTrue(any("capacities" in e for e in qual.requested_errors(r, args, [0])))

    def test_cpu_population_and_migration(self):
        r = report()
        self.assertTrue(qual.base_errors(r, 4, "all", True))
        r["per_cpu"].append(copy.deepcopy(r["per_cpu"][0]))
        self.assertTrue(qual.base_errors(r, 2, "migrate", True))


if __name__ == "__main__":
    unittest.main()
