#!/usr/bin/env python3
"""Portable gate tests using hand-counted synthetic bench reports. Refs #871."""
import copy
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import tempfile
import sys
import json

spec = importlib.util.spec_from_file_location("transport_qualification", Path(__file__).parent / "bench_nmi_transport/qualify.py")
qual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qual)
EXPECTED_CPU_FIELDS = ("user", "nice", "system", "irq", "softirq", "steal", "idle", "iowait")


def report():
    # One CPU: DATA attempts 1..3; two boundaries, all delivered in order.
    r = {
        "accepted": True, "stage": "complete", "libbpf_error": 0,
        "bad_records": 0, "accounting_ok": True, "seq_reorders": 0,
        "seal_violations": 0, "timestamp_regressions": 0, "lost_notifications": 0,
        "attempts": 5, "participating_busy_cpu_ns": 50, "elapsed": 1, "hits": 3, "migrations": 0,
        "pause_drain": False, "buffer_bytes": [67108864], "boundary_offset_ns": 0,
        "boundary_fires": 2, "boundary_outputs": 2, "wrong_cpu_events": 0,
        "duration": 1, "period_ns": 10_000_000, "clock_ticks": 1_000_000_000, "process_cpu_ns": 50,
        "per_cpu": [{"cpu": 0, "max_boundary_gap_ns": 100_000_000,
                     "syscall_fires": 2, "syscall_outputs": 2, "wrong_cpu_events": 0,
                     "helper_tid": 123, "helper_cpu_ns": 100_000,
                     "helper_wakeups": 2, "helper_syscalls": 2, "boundary_expected": 100,
                     "hog_tid": 0, "hog_iterations": 0,
                     "frontier_ns": 900_000_000, "frontier_seen_ns": 910_000_000,
                     "max_frontier_gap_ns": 100_000_000, "max_frontier_stamp_gap_ns": 10_000_000,
                     "max_boundary_stamp_gap_ns": 10_000_000, "sites": [
                         {"site": "DATA", "produced": 3, "successes": 3, "failures": 0,
                          "delivered": 3, "loss": 0, "seq_gaps": 0, "reconciled": True},
                         {"site": "BOUNDARY", "produced": 2, "successes": 2, "failures": 0,
                          "delivered": 2, "loss": 0, "seq_gaps": 0, "reconciled": True}]}]
    }

    r["non_bench_process_cpu"] = qual.process_deltas(
        {"records": {(10, 1): {"comm": "other", "ticks": 0}}, "unreadable": 0, "observed_ns": 1},
        {"records": {(10, 1): {"comm": "other", "ticks": 1}}, "unreadable": 0, "observed_ns": 2},
        20, r["clock_ticks"])
    set_busy(r, r["participating_busy_cpu_ns"])
    return r


def set_busy(r, value):
    r["participating_busy_cpu_ns"] = value
    r["participating_busy_ticks_start"] = 0
    r["participating_busy_ticks_end"] = value
    fields = dict.fromkeys(EXPECTED_CPU_FIELDS, 0)
    end = dict(fields, user=value)
    row = {"start": fields, "end": end, "delta": end.copy()}
    r["proc_stat_participating_ticks"] = copy.deepcopy(row)
    r["proc_stat_whole_box_ticks"] = copy.deepcopy(row)
    for c in r["per_cpu"]:
        c["proc_stat_ticks"] = copy.deepcopy(row)
    # Distribute this synthetic total over CPUs without rounding it away.
    for c in r["per_cpu"][1:]:
        c["proc_stat_ticks"]["end"]["user"] = c["proc_stat_ticks"]["delta"]["user"] = 0


def add_busy(r, value):
    set_busy(r, r["participating_busy_cpu_ns"] + value)


def quartet(case="all"):
    runs = {arm: report() for arm in qual.ARMS}
    for arm, run in runs.items():
        if arm not in qual.SYSCALL_ARMS:
            run["per_cpu"][0].update(helper_tid=0, helper_cpu_ns=0, helper_wakeups=0,
                                     helper_syscalls=0, syscall_fires=0, syscall_outputs=0)
        if arm in ("shared", "perf"):
            run["per_cpu"][0]["sites"][1].update(produced=0, successes=0, delivered=0)
            run.update(boundary_fires=0, boundary_outputs=0)
        if case in ("idle", "hog"):
            run["hits"] = 0
            run["per_cpu"][0]["sites"][0].update(produced=0, successes=0, delivered=0)
        if case == "hog":
            run["per_cpu"][0].update(hog_tid=321, hog_iterations=1000)
        if arm in ("shared", "perf"):
            run["period_ns"] = 0
            run["per_cpu"][0]["boundary_expected"] = 0
        run["attempts"] = sum(s["produced"] for _, s in qual.sites(run))
    return runs


class Gates(unittest.TestCase):
    def test_period_grid_runs_all_five_cases_and_each_demand_cadence(self):
        for flags, periods in (([], [10_000_000]), (["--periods", "10000000", "40000000"], [10_000_000, 40_000_000]),
                               (["--period-ns", "20000000"], [20_000_000])):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp) / "qualification"
                calls = []
                def run_one(destination, label, args):
                    calls.append((label, args))
                    return {}
                with mock.patch.object(sys, "argv", ["qualify.py", "--out", str(out), "--repeats", "1"] + flags), \
                     mock.patch.object(qual.platform, "system", return_value="Linux"), \
                     mock.patch.object(qual, "capture", return_value={"exit": 0}), \
                     mock.patch.object(qual, "metadata", return_value={"allowed_cpus": [0, 1, 2, 3], "irq_time_accounting": True}), \
                     mock.patch.object(qual, "run_one", side_effect=run_one), \
                     mock.patch.object(qual, "control_errors", return_value=[]), \
                     mock.patch.object(qual, "evaluate_runs", return_value={"status": "PASS"}), \
                     mock.patch("builtins.print"):
                    self.assertEqual(qual.main(), 0)
                cells = json.loads((out / "summary.json").read_text())["cells"]
                self.assertEqual(len(cells), 3 * 5 * len(periods))
                self.assertEqual(set(qual.CASES), {"idle", "one", "all", "migrate", "hog"})
                for population in ("1", "4", "all"):
                    for case in qual.CASES:
                        for period in periods:
                            prefix = f"cpu-{population}-{case}-p{period}-r1"
                            for arm in qual.ARMS:
                                args = next(args for label, args in calls if label == prefix + "-" + arm)
                                self.assertEqual(args[args.index("--case") + 1], case)
                                if arm in qual.BOUNDARY_ARMS:
                                    self.assertEqual(int(args[args.index("--period-ns") + 1]), period)
                                self.assertEqual("--syscall-demand" in args, arm == "syscall_demand")
                                self.assertEqual("--syscall-boundary" in args, arm == "syscall_boundary")
                            summary = json.loads((out / (prefix + ".summary.json")).read_text())
                            self.assertEqual(set(summary["controls"]), {"loss", "seal", "syscall_seal"})
                            self.assertEqual(summary["period_ns"], period)
        for flags in (["--periods", "0"], ["--periods", "-1"], ["--periods", "10", "10"],
                      ["--periods", "10000000", "--period-ns", "20000000"]):
            with mock.patch.object(sys, "argv", ["qualify.py", "--out", "/tmp/unused"] + flags), \
                 mock.patch.object(qual, "capture") as capture, mock.patch.object(sys, "stderr"), \
                 self.assertRaises(SystemExit):
                qual.main()
            capture.assert_not_called()

    def test_demand_wrong_treatment_and_run_capture(self):
        r = report()
        r.update(transport="perf", case="all", cpus=1, duration=30,
                 period_ns=10_000_000, buffer_bytes_total=67108864, boundary_source="syscall_demand")
        args = ["--transport", "perf", "--case", "all", "--cpus", "1", "--duration", "30",
                "--period-ns", "10000000", "--bytes", "67108864", "--syscall-demand"]
        self.assertEqual(qual.requested_errors(r, args, [0]), [])
        for field, value in (("boundary_source", "syscall"), ("period_ns", 20_000_000)):
            broken = copy.deepcopy(r)
            broken[field] = value
            self.assertTrue(any(field in e for e in qual.requested_errors(broken, args, [0])))
        snapshot = {"records": {(10, 1): {"comm": "other", "ticks": 0}}, "observed_ns": 1, "unreadable": 0}
        after = copy.deepcopy(snapshot)
        after["records"][(10, 1)]["ticks"] = 1
        after["observed_ns"] = 2
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(qual, "process_snapshot", side_effect=[snapshot, after]), \
             mock.patch.object(qual.os, "sched_getaffinity", return_value={0}, create=True), \
             mock.patch.object(qual.subprocess, "Popen") as popen:
            process = popen.return_value.__enter__.return_value
            process.pid, process.returncode = 20, 0
            process.communicate.return_value = (json.dumps(r), "")
            actual = qual.run_one(Path(tmp), "demand", args)
            self.assertEqual(actual["non_bench_process_cpu"]["bench_pid"], 20)
            self.assertEqual(actual["non_bench_process_cpu"]["top3"][0]["cpu_ticks_delta"], 1)
            raw = json.loads((Path(tmp) / "demand.raw.json").read_text())
            self.assertEqual(raw["non_bench_process_cpu"], actual["non_bench_process_cpu"])
            self.assertEqual(raw["command"][-1], "--syscall-demand")

    def test_frontier_gate_keeps_boundary_only_failures_visible(self):
        for arm in qual.BOUNDARY_ARMS:
            for field in ("max_frontier_gap_ns", "max_frontier_stamp_gap_ns"):
                runs = quartet()
                cpu = runs[arm]["per_cpu"][0]
                # Old failures remain in the report; dense qualified DATA can
                # supply progress independently of the explicit producer.
                cpu.update(max_boundary_gap_ns=165_600_000, max_boundary_stamp_gap_ns=151_900_000)
                result = qual.evaluate_runs(runs, 1, "all", True)
                self.assertEqual(result["status"], "PASS")
                self.assertEqual(cpu["max_boundary_gap_ns"], 165_600_000)
                cpu[field] = qual.MAX_GAP_NS
                self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
                cpu[field] += 1
                result = qual.evaluate_runs(runs, 1, "all", True)
                self.assertIn(arm + ": CPU 0: frontier gap exceeds 100 ms", result["errors"])

    def test_hog_has_a_loop_on_each_cpu_and_no_watchpoint_data(self):
        runs = quartet("hog")
        self.assertEqual(qual.evaluate_runs(runs, 1, "hog", True)["status"], "PASS")
        self.assertIsNone(qual.evaluate_runs(runs, 1, "hog", True)["perf_shared_cpu_ns_per_attempt_ratio"])
        for arm in qual.ARMS:
            for field in ("hog_tid", "hog_iterations"):
                broken = copy.deepcopy(runs)
                broken[arm]["per_cpu"][0][field] = 0
                self.assertIn(arm + ": CPU 0: untraced hog did not run",
                              qual.evaluate_runs(broken, 1, "hog", True)["errors"])
            broken = copy.deepcopy(runs)
            broken[arm]["per_cpu"][0]["sites"][0].update(produced=1, successes=1, delivered=1)
            broken[arm]["hits"] = 1
            self.assertIn(arm + ": hog case produced DATA", qual.evaluate_runs(broken, 1, "hog", True)["errors"])
        for arm in qual.BOUNDARY_ARMS:
            broken = copy.deepcopy(runs)
            broken[arm]["per_cpu"][0]["max_frontier_gap_ns"] += 1
            self.assertIn(arm + ": CPU 0: frontier gap exceeds 100 ms",
                          qual.evaluate_runs(broken, 1, "hog", True)["errors"])

    def test_demand_can_skip_syscalls_but_must_wake_and_keep_frontier_current(self):
        runs = quartet()
        r = runs["syscall_demand"]
        r.update(boundary_fires=0, boundary_outputs=0)
        cpu = r["per_cpu"][0]
        cpu.update(syscall_fires=0, syscall_outputs=0, helper_syscalls=0)
        cpu["sites"][1].update(produced=0, successes=0, delivered=0)
        result = qual.evaluate_runs(runs, 1, "all", True)
        self.assertEqual(result["status"], "PASS")
        cadence = result["boundary_cadence_per_cpu"]
        self.assertEqual(cadence["syscall_demand"]["0"], {
            "fires": 0, "expected": 100, "fires_per_expected": 0,
            "helper_wakeups": 2, "helper_syscalls": 0})
        self.assertEqual(cadence["syscall_boundary"]["0"]["fires"], 2)
        self.assertEqual(cadence["syscall_boundary"]["0"]["fires_per_expected"], 0.02)
        for field, value, message in (
                ("helper_wakeups", 0, "syscall helper/counter mismatch"),
                ("helper_syscalls", 1, "syscall helper/counter mismatch"),
                ("boundary_expected", 99, "boundary expected-period count mismatch"),
                ("max_frontier_gap_ns", qual.MAX_GAP_NS + 1, "frontier gap exceeds 100 ms")):
            broken = copy.deepcopy(runs)
            broken["syscall_demand"]["per_cpu"][0][field] = value
            self.assertTrue(any(message in e for e in qual.evaluate_runs(broken, 1, "all", True)["errors"]))
        # With no DATA, absent boundary delivery cannot pass even if the
        # producer reports successful helper calls.
        for case in ("idle", "hog"):
            broken = quartet(case)
            b = broken["syscall_demand"]["per_cpu"][0]["sites"][1]
            b.update(delivered=0, loss=2, seq_gaps=2)
            errors = qual.evaluate_runs(broken, 1, case, True)["errors"]
            self.assertIn("syscall_demand: CPU 0: boundary outputs != consumer-delivered BOUNDARY", errors)

    def test_every_boundary_arm_reconciles_consumer_delivery(self):
        for arm in qual.BOUNDARY_ARMS:
            runs = quartet()
            r = runs[arm]
            self.assertEqual(r["boundary_fires"], r["boundary_outputs"])
            r["per_cpu"][0]["sites"][1].update(delivered=1, loss=1, seq_gaps=1)
            self.assertIn(arm + ": CPU 0: boundary outputs != consumer-delivered BOUNDARY",
                          qual.evaluate_runs(runs, 1, "all", True)["errors"])

    def test_demand_preserves_syscall_safety_on_each_cpu(self):
        runs = quartet()
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, helper_tid=124 if second["helper_tid"] else 0)
            r["per_cpu"].append(second)
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("hits", "attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        self.assertEqual(qual.evaluate_runs(runs, 2, "all", True)["status"], "PASS")
        for field, value, message in (
                ("wrong_cpu_events", 1, "wrong-CPU syscall boundary event"),
                ("helper_tid", 0, "syscall helper/counter mismatch"),
                ("syscall_fires", 0, "syscall helper/counter mismatch"),
                ("syscall_outputs", 0, "syscall helper/counter mismatch"),
                ("max_frontier_gap_ns", qual.MAX_GAP_NS + 1, "CPU 1: frontier gap exceeds 100 ms"),
                ("max_frontier_stamp_gap_ns", qual.MAX_GAP_NS + 1, "CPU 1: frontier gap exceeds 100 ms"),
                ("seal_violations", 1, "seal violation or timestamp regression"),
                ("timestamp_regressions", 1, "seal violation or timestamp regression")):
            broken = copy.deepcopy(runs)
            broken["syscall_demand"]["per_cpu"][1][field] = value
            self.assertTrue(any(message in e for e in qual.evaluate_runs(broken, 2, "all", True)["errors"]), field)
        for field, value, message in (
                ("wrong_cpu_events", 1, "wrong-CPU syscall boundary event"),
                ("boundary_fires", 5, "syscall boundary fires != outputs"),
                ("boundary_outputs", 5, "syscall boundary fires != outputs"),
                ("lost_notifications", 1, "positive PERF_RECORD_LOST")):
            broken = copy.deepcopy(runs)
            broken["syscall_demand"][field] = value
            self.assertTrue(any(message in e for e in qual.evaluate_runs(broken, 2, "all", True)["errors"]), field)
        broken = copy.deepcopy(runs)
        broken["syscall_demand"].update(boundary_fires=6, boundary_outputs=6)
        self.assertIn("syscall_demand: syscall boundary aggregate/per-CPU counter mismatch",
                      qual.evaluate_runs(broken, 2, "all", True)["errors"])

        runs = quartet("hog")
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, hog_tid=322, helper_tid=124 if second["helper_tid"] else 0)
            r["per_cpu"].append(second)
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        self.assertEqual(qual.evaluate_runs(runs, 2, "hog", True)["status"], "PASS")
        runs["syscall_demand"]["per_cpu"][1]["hog_iterations"] = 0
        self.assertIn("syscall_demand: CPU 1: untraced hog did not run",
                      qual.evaluate_runs(runs, 2, "hog", True)["errors"])

    def test_new_per_cpu_evidence_fields_fail_closed(self):
        fields = ("helper_cpu_ns", "helper_wakeups", "helper_syscalls", "boundary_expected",
                  "hog_tid", "hog_iterations", "frontier_ns", "frontier_seen_ns",
                  "max_frontier_gap_ns", "max_frontier_stamp_gap_ns",
                  "max_boundary_gap_ns", "max_boundary_stamp_gap_ns")
        for field in fields:
            for value in (None, -1):
                with self.subTest(field=field, value=value):
                    runs = quartet()
                    runs["syscall_demand"]["per_cpu"][0][field] = value
                    self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")
            runs = quartet()
            del runs["syscall_demand"]["per_cpu"][0][field]
            self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")

    def test_cpu_report_preserves_each_field_and_rejects_inconsistent_snapshots(self):
        self.assertEqual(set(qual.CPU_FIELDS), set(EXPECTED_CPU_FIELDS))
        runs = quartet()
        result = qual.evaluate_runs(runs, 1, "all", True)
        for arm in qual.ARMS:
            cpu = result["cpu_accounting"][arm]
            self.assertEqual(cpu["bench_process_cpu_ns"], 50)
            self.assertEqual(cpu["helper_cpu_ns_per_cpu"], {"0": 100_000 if arm in qual.SYSCALL_ARMS else 0})
            self.assertEqual(cpu["non_bench_process_cpu"]["top3"][0]["cpu_ticks_delta"], 1)
            self.assertEqual(cpu["proc_stat_ticks_per_cpu"]["0"]["delta"]["user"], 50)
        for location in ("proc_stat_whole_box_ticks", "proc_stat_participating_ticks", "per_cpu"):
            for phase in ("start", "end", "delta"):
                for field in EXPECTED_CPU_FIELDS:
                    with self.subTest(location=location, phase=phase, field=field):
                        broken = copy.deepcopy(runs)
                        r = broken["syscall_boundary"]
                        row = r["per_cpu"][0]["proc_stat_ticks"] if location == "per_cpu" else r[location]
                        row[phase][field] += 1
                        self.assertEqual(qual.evaluate_runs(broken, 1, "all", True)["status"], "FAIL")
        for field in ("process_cpu_ns", "clock_ticks", "elapsed", "participating_busy_cpu_ns",
                      "participating_busy_ticks_start", "participating_busy_ticks_end",
                      "proc_stat_whole_box_ticks", "proc_stat_participating_ticks", "non_bench_process_cpu"):
            broken = copy.deepcopy(runs)
            del broken["perf"][field]
            self.assertEqual(qual.evaluate_runs(broken, 1, "all", True)["status"], "FAIL", field)
        # Correctly differenced but wrongly summed CPU fields also fail.
        broken = copy.deepcopy(runs)
        row = broken["perf"]["proc_stat_participating_ticks"]
        row["end"]["irq"] = row["delta"]["irq"] = 1
        self.assertIn("perf: /proc/stat participating sum mismatch",
                      qual.evaluate_runs(broken, 1, "all", True)["errors"])

    def test_demand_uses_the_same_one_percent_quantity_and_helper_gate(self):
        for irq in (True, False):
            runs = quartet("idle")
            add_busy(runs["syscall_demand"], 10_000_000)
            runs["syscall_demand"]["per_cpu"][0]["helper_cpu_ns"] = 10_000_000
            result = qual.evaluate_runs(runs, 1, "idle", irq)
            gate = result["boundary_cpu_gates"]["syscall_demand"]
            self.assertEqual(gate["proc_stat_increment_core_fraction_per_participating_cpu"], 0.01)
            self.assertNotIn("core_fraction_per_cpu", gate)
            self.assertEqual(gate["helper_core_fraction_per_cpu"], {"0": 0.01})
            self.assertEqual(gate["status"], "PASS" if irq else "UNMEASURED")
            broken = copy.deepcopy(runs)
            broken["syscall_demand"]["per_cpu"][0]["helper_cpu_ns"] += 1
            self.assertIn("syscall_demand: helper CPU exceeds 1% on a CPU",
                          qual.evaluate_runs(broken, 1, "idle", irq)["errors"])
            if irq:
                add_busy(runs["syscall_demand"], 1)
                self.assertIn("syscall_demand: boundary-only idle-box incremental CPU exceeds 1% per CPU",
                              qual.evaluate_runs(runs, 1, "idle", irq)["errors"])

    def test_top_three_processes_handle_reuse_exit_and_non_bench_filter(self):
        def snap(rows, observed):
            return {"records": {identity: {"comm": str(identity), "ticks": ticks} for identity, ticks in rows},
                    "unreadable": 0, "observed_ns": observed}
        before = snap([((1, 1), 1), ((2, 2), 2), ((3, 3), 3), ((4, 4), 4), ((5, 5), 5), ((9, 9), 0)], 10)
        after = snap([((1, 1), 11), ((2, 2), 32), ((3, 3), 23), ((4, 4), 44), ((5, 6), 10000), ((8, 8), 10000)], 20)
        result = qual.process_deltas(before, after, 4, 100)
        self.assertEqual([r["pid"] for r in result["top3"]], [2, 3, 1])
        self.assertEqual([r["cpu_ns_delta"] for r in result["top3"]], [300_000_000, 200_000_000, 100_000_000])
        self.assertEqual((result["started"], result["exited"], result["matched_non_bench"]), (2, 2, 3))
        runs = quartet()
        for field in ("pid", "starttime_ticks", "comm", "cpu_ticks_start", "cpu_ticks_end", "cpu_ticks_delta", "cpu_ns_delta"):
            broken = copy.deepcopy(runs)
            del broken["perf"]["non_bench_process_cpu"]["top3"][0][field]
            self.assertEqual(qual.evaluate_runs(broken, 1, "all", True)["status"], "FAIL", field)
        for field in ("clock_ticks", "bench_pid", "before_ns", "after_ns", "matched_non_bench", "started",
                      "exited", "unreadable_before", "unreadable_after", "coverage", "top3"):
            broken = copy.deepcopy(runs)
            del broken["perf"]["non_bench_process_cpu"][field]
            self.assertEqual(qual.evaluate_runs(broken, 1, "all", True)["status"], "FAIL", field)
        for value in (20, 0):
            broken = copy.deepcopy(runs)
            broken["perf"]["non_bench_process_cpu"]["top3"][0]["pid"] = value
            self.assertIn("perf: invalid non-bench process CPU delta", qual.evaluate_runs(broken, 1, "all", True)["errors"])
        after["records"][(1, 1)]["ticks"] = 0
        with self.assertRaisesRegex(ValueError, "regressed"):
            qual.process_deltas(before, after, 4, 100)

    def test_proc_stat_parser_handles_spaces_parentheses_and_vanished_pids(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            (proc / "123").mkdir()
            # Fields 3..22; comm can contain spaces and right parentheses.
            fields = ["S"] + ["0"] * 19
            fields[11], fields[12], fields[19] = "7", "8", "99"
            (proc / "123/stat").write_text("123 (a ) name) " + " ".join(fields))
            (proc / "124").mkdir()
            (proc / "uptime").write_text("ignored")
            result = qual.process_snapshot(proc)
            self.assertEqual(result["records"], {(123, 99): {"comm": "a ) name", "ticks": 15}})
            self.assertEqual(result["unreadable"], 1)
            fields[11] = "bad"
            (proc / "123/stat").write_text("123 (a) " + " ".join(fields))
            result = qual.process_snapshot(proc)
            self.assertEqual(result["records"], {})
            self.assertEqual(result["unreadable"], 2)

    def test_shared_loss_is_a_measurement_perf_loss_fails_cell(self):
        runs = quartet()
        runs["shared"]["per_cpu"][0]["sites"][0].update(
            successes=2, failures=1, delivered=2, loss=1, seq_gaps=1)
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        # Even shared reconciliation/LOST diagnostics cannot become a loss gate.
        runs["shared"].update(accounting_ok=False, lost_notifications=1)
        runs["shared"]["per_cpu"][0]["sites"][0].update(reconciled=False, seq_gaps=0)
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        for arm in ("perf", "perf_boundary", "syscall_boundary"):
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

    def test_syscall_boundary_all_cases_each_cpu_deadlines_and_fires(self):
        for case in qual.CASES:
            # Reuse the one-CPU hand-counted stream. A one-CPU migration is
            # necessarily pinned. Idle has boundaries but zero DATA.
            runs = quartet(case)
            self.assertEqual(qual.evaluate_runs(runs, 1, case, True)["status"], "PASS", case)
            for field in ("max_frontier_gap_ns", "max_frontier_stamp_gap_ns"):
                good = copy.deepcopy(runs)
                good["syscall_boundary"]["per_cpu"][0][field] = 100_000_000
                self.assertEqual(qual.evaluate_runs(good, 1, case, True)["status"], "PASS")
                good["syscall_boundary"]["per_cpu"][0][field] += 1
                result = qual.evaluate_runs(good, 1, case, True)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("syscall_boundary: CPU 0: frontier gap exceeds 100 ms", result["errors"])
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0]["sites"][1].update(
                successes=1, failures=1, delivered=1, loss=1, seq_gaps=1)
            result = qual.evaluate_runs(broken, 1, case, True)
            self.assertIn("syscall_boundary: CPU 0: boundary fires != outputs", result["errors"])
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0]["sites"][1].update(
                produced=0, successes=0, delivered=0)
            self.assertTrue(any("boundary did not fire" in e for e in
                                qual.evaluate_runs(broken, 1, case, True)["errors"]))

    def test_syscall_wrong_cpu_and_counter_crosschecks(self):
        runs = quartet()
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        for target in ("aggregate", "per_cpu"):
            broken = copy.deepcopy(runs)
            r = broken["syscall_boundary"]
            (r if target == "aggregate" else r["per_cpu"][0])["wrong_cpu_events"] = 1
            self.assertIn("syscall_boundary: wrong-CPU syscall boundary event",
                          qual.evaluate_runs(broken, 1, "all", True)["errors"])
        for field in ("boundary_fires", "boundary_outputs"):
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"][field] += 1
            self.assertIn("syscall_boundary: syscall boundary fires != outputs",
                          qual.evaluate_runs(broken, 1, "all", True)["errors"])
        broken = copy.deepcopy(runs)
        broken["syscall_boundary"].update(boundary_fires=3, boundary_outputs=3)
        self.assertIn("syscall_boundary: syscall boundary aggregate/per-CPU counter mismatch",
                      qual.evaluate_runs(broken, 1, "all", True)["errors"])
        for field in ("helper_tid", "syscall_fires", "syscall_outputs", "helper_syscalls", "helper_wakeups"):
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0][field] = 0
            self.assertIn("syscall_boundary: CPU 0: syscall helper/counter mismatch",
                          qual.evaluate_runs(broken, 1, "all", True)["errors"])

    def test_syscall_seals_regressions_and_loss_notifications(self):
        for case in qual.CASES:
            for field in ("seal_violations", "timestamp_regressions"):
                for local in (False, True):
                    runs = quartet(case)
                    r = runs["syscall_boundary"]
                    self.assertEqual(qual.evaluate_runs(runs, 1, case, True)["status"], "PASS")
                    (r["per_cpu"][0] if local else r)[field] = 1
                    self.assertIn("syscall_boundary: seal violation or timestamp regression",
                                  qual.evaluate_runs(runs, 1, case, True)["errors"])
        runs = quartet()
        runs["syscall_boundary"]["lost_notifications"] = 1
        self.assertIn("syscall_boundary: positive PERF_RECORD_LOST (reported separately)",
                      qual.evaluate_runs(runs, 1, "all", True)["errors"])

    def test_syscall_cost_limit_and_measurable_helper_without_irq_accounting(self):
        runs = quartet("idle")
        add_busy(runs["syscall_boundary"], 10_000_000)
        runs["syscall_boundary"]["per_cpu"][0]["helper_cpu_ns"] = 10_000_000
        result = qual.evaluate_runs(runs, 1, "idle", True)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["boundary_cpu_gates"]["syscall_boundary"]["status"], "PASS")
        result = qual.evaluate_runs(runs, 1, "idle", False)
        self.assertEqual(result["status"], "VOID")
        self.assertEqual(result["boundary_cpu_gates"]["syscall_boundary"]["status"], "UNMEASURED")
        self.assertEqual(result["syscall_helper_core_fraction_per_cpu"]["syscall_boundary"], {"0": 0.01})
        # One nanosecond over either limit fails; the measured helper lower
        # bound cannot pass by hiding behind missing IRQ accounting.
        for irq in (True, False):
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0]["helper_cpu_ns"] += 1
            result = qual.evaluate_runs(broken, 1, "idle", irq)
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("syscall_boundary: helper CPU exceeds 1% on a CPU", result["errors"])
        add_busy(runs["syscall_boundary"], 1)
        result = qual.evaluate_runs(runs, 1, "idle", True)
        self.assertIn("syscall_boundary: boundary-only idle-box incremental CPU exceeds 1% per CPU", result["errors"])
        runs = quartet("idle")
        runs["syscall_boundary"]["per_cpu"][0]["helper_cpu_ns"] = -1
        self.assertIn("syscall_boundary: invalid helper_cpu_ns",
                      qual.evaluate_runs(runs, 1, "idle", True)["errors"])

    def test_syscall_helper_cost_cannot_be_averaged_away(self):
        runs = quartet("idle")
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, helper_tid=124)
            r["per_cpu"].append(second)
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        runs["syscall_boundary"]["per_cpu"][1]["helper_cpu_ns"] = 10_000_000
        self.assertEqual(qual.evaluate_runs(runs, 2, "idle", True)["status"], "PASS")
        runs["syscall_boundary"]["per_cpu"][1]["helper_cpu_ns"] += 1
        self.assertIn("syscall_boundary: helper CPU exceeds 1% on a CPU",
                      qual.evaluate_runs(runs, 2, "idle", True)["errors"])

    def test_syscall_gates_do_not_hide_a_second_cpu_failure(self):
        runs = quartet()
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, helper_tid=124)
            r["per_cpu"].append(second)
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("hits", "attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        self.assertEqual(qual.evaluate_runs(runs, 2, "all", True)["status"], "PASS")
        runs["syscall_boundary"]["per_cpu"][1]["max_frontier_gap_ns"] += 1
        self.assertIn("syscall_boundary: CPU 1: frontier gap exceeds 100 ms",
                      qual.evaluate_runs(runs, 2, "all", True)["errors"])

    def test_adversarial_cpu_is_measured_but_only_normal_boundary_is_cpu_gated(self):
        runs = quartet("idle")
        add_busy(runs["adversarial"], 500_000_000)
        result = qual.evaluate_runs(runs, 1, "idle", True)
        self.assertEqual(result["status"], "PASS")
        self.assertGreater(result["proc_stat_increment_core_fraction_per_participating_cpu"]["adversarial"], 0.01)
        add_busy(runs["perf_boundary"], 10_000_000)
        self.assertEqual(qual.evaluate_runs(runs, 1, "idle", True)["status"], "PASS")
        add_busy(runs["perf_boundary"], 1)
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
        adversarial["per_cpu"][0]["max_frontier_gap_ns"] += 1
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        adversarial["per_cpu"][0]["seal_violations"] = 1
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")

    def test_boundary_deadline_equality_and_one_ns_over(self):
        r = report()
        self.assertEqual(qual.normal_errors(r, 1, "all", True), [])
        r["per_cpu"][0]["max_frontier_gap_ns"] += 1
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
        perf.update(participating_busy_cpu_ns=55)
        self.assertAlmostEqual(qual.cpu_cost_ratio(perf, shared), 1.10)
        perf["attempts"] = 10
        self.assertAlmostEqual(qual.cpu_cost_ratio(perf, shared), 0.55)
        perf["attempts"] = 0
        self.assertIsNone(qual.cpu_cost_ratio(perf, shared))
        baseline, boundary = report(), report()
        baseline.update(participating_busy_cpu_ns=200_000_000, elapsed=2)
        boundary.update(participating_busy_cpu_ns=240_000_000, elapsed=2)
        self.assertAlmostEqual(qual.idle_increment(boundary, baseline, 2), 0.01)
        boundary["participating_busy_cpu_ns"] += 4
        self.assertGreater(qual.idle_increment(boundary, baseline, 2), 0.01)

    def test_wrong_treatment_is_void(self):
        r = report()
        r.update(transport="perf", case="all", cpus=1, duration=30,
                 period_ns=100_000, buffer_bytes_total=67108864, boundary_source="perf")
        args = ["--transport", "perf", "--case", "all", "--cpus", "1", "--duration", "30",
                "--period-ns", "100000", "--bytes", "67108864"]
        self.assertEqual(qual.requested_errors(r, args, [0]), [])
        r["period_ns"] = 10_000_000
        self.assertTrue(any("period_ns" in e for e in qual.requested_errors(r, args, [0])))
        r["period_ns"] = 100_000
        self.assertTrue(any("CPU identities" in e for e in qual.requested_errors(r, args, [1])))
        r["buffer_bytes"] = [4096]
        self.assertTrue(any("capacities" in e for e in qual.requested_errors(r, args, [0])))
        r["buffer_bytes"] = [67108864]
        self.assertTrue(any("boundary_source" in e for e in
                            qual.requested_errors(r, args + ["--syscall-boundary"], [0])))
        r["boundary_source"] = "syscall"
        self.assertEqual(qual.requested_errors(r, args + ["--syscall-boundary"], [0]), [])

    def test_cpu_population_and_migration(self):
        r = report()
        self.assertTrue(qual.base_errors(r, 4, "all", True))
        r["per_cpu"].append(copy.deepcopy(r["per_cpu"][0]))
        self.assertTrue(qual.base_errors(r, 2, "migrate", True))


if __name__ == "__main__":
    unittest.main()
