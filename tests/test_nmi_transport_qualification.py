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
        "boundary_source": "perf", "request_after_ms": 50, "coverage_ms": 0, "phase_jitter": False,
        "cpu_accounting_elapsed_ns": 1_000_000_000, "schedstat_elapsed_ns": 1_000_000_000, "online_cpus": 1, "max_drain_gap_ms": 10,
        "held_data": "UNMEASURED",
        "duration": 1, "period_ns": 10_000_000, "clock_ticks": 1_000_000_000, "process_cpu_ns": 50,
        "per_cpu": [{"cpu": 0, "max_boundary_gap_ns": 100_000_000,
                     "syscall_fires": 2, "syscall_outputs": 2, "wrong_cpu_events": 0,
                     "helper_tid": 123, "helper_cpu_ns": 100_000,
                     "schedstat_running_ns": {"status": "MEASURED", "start": 100, "end": 150, "delta": 50},
                     "requests": 0, "request_acknowledgements": 0, "request_outstanding": False,
                     "max_request_to_frontier_ns": 0, "request_to_frontier_samples_ns": [], "request_outstanding_age_ns": 0, "phase_offset_ns": 0,
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
    if case == "hog_data":
        return hog_data_runs()
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
        if arm == "syscall_request":
            run["boundary_source"] = "syscall_request"
            cpu = run["per_cpu"][0]
            cpu.update(boundary_expected=0, requests=2, request_acknowledgements=2,
                       max_request_to_frontier_ns=1_000_000, request_to_frontier_samples_ns=[500_000, 1_000_000])
            data = cpu["sites"][0]["delivered"]
            run["held_data"] = {"arrived": data, "released": data, "queue_depth": 0,
                                "queue_max_depth": 1 if data else 0, "queue_growth_records": 0,
                                "max_queue_growth_per_drain": 0, "queue_growth_records_per_second": 0,
                                "max_held_data_age_ns": 50_000_000 if data else 0, "oldest_pending_age_ns": 0,
                                "held_age_samples": data, "p99_held_data_age_ns": 50_000_000 if data else 0}
            if case in ("idle", "hog"):
                cpu.update(helper_wakeups=0, helper_syscalls=0, syscall_fires=0, syscall_outputs=0,
                           requests=0, request_acknowledgements=0, max_request_to_frontier_ns=0,
                           request_to_frontier_samples_ns=[])
                cpu["sites"][1].update(produced=0, successes=0, delivered=0)
                run.update(boundary_fires=0, boundary_outputs=0)
        run["attempts"] = sum(s["produced"] for _, s in qual.sites(run))
    return runs


def hog_data_runs(cpus=2):
    assert cpus >= 2
    runs = quartet("all")
    for arm, r in runs.items():
        first = r["per_cpu"][0]
        r["per_cpu"] = []
        for i in range(cpus):
            c = copy.deepcopy(first)
            c.update(cpu=2 + 3*i, helper_tid=123+i if first["helper_tid"] else 0)
            if i >= (cpus + 1) // 2:
                c.update(hog_tid=321+i, hog_iterations=1000)
                c["sites"][0].update(produced=0, successes=0, delivered=0)
            r["per_cpu"].append(c)
        r["online_cpus"] = cpus
        r["hits"] = 3 * ((cpus + 1) // 2)
        r["attempts"] = sum(site["produced"] for _, site in qual.sites(r))
        r["boundary_fires"] = sum(c["sites"][1]["produced"] for c in r["per_cpu"])
        r["boundary_outputs"] = r["boundary_fires"]
        if arm == "syscall_request":
            r["held_data"].update(arrived=r["hits"], released=r["hits"], held_age_samples=r["hits"])
        set_busy(r, 50)
    return runs


class Gates(unittest.TestCase):
    def test_hog_data_roles_every_arm_and_odd_population(self):
        for cpus in (2, 3, 4, 5):
            runs = hog_data_runs(cpus)
            result = qual.evaluate_runs(runs, cpus, "hog_data", True)
            self.assertEqual(result["status"], "PASS", result["errors"])
            for arm in qual.ARMS:
                for i in range(cpus):
                    broken = copy.deepcopy(runs)
                    c = broken[arm]["per_cpu"][i]
                    if i < (cpus + 1) // 2:
                        c["sites"][0].update(produced=0, successes=0, delivered=0)
                        message = f"CPU {c['cpu']}: active writer did not fire"
                    else:
                        c["sites"][0].update(produced=1, successes=1, delivered=1)
                        message = f"CPU {c['cpu']}: starved CPU produced DATA"
                    errors = qual.evaluate_runs(broken, cpus, "hog_data", True)["errors"]
                    self.assertIn(arm + ": " + message, errors)
                for i in range((cpus + 1) // 2, cpus):
                    for field in ("hog_tid", "hog_iterations"):
                        broken = copy.deepcopy(runs)
                        c = broken[arm]["per_cpu"][i]
                        c[field] = 0
                        self.assertIn(f"{arm}: CPU {c['cpu']}: untraced hog did not run",
                                      qual.evaluate_runs(broken, cpus, "hog_data", True)["errors"])
            # Remove a starved CPU's request with all request/boundary counters
            # still consistent: other CPUs' requests cannot certify this CPU.
            for i in range((cpus + 1) // 2, cpus):
                broken = copy.deepcopy(runs)
                r = broken["syscall_request"]
                c = r["per_cpu"][i]
                c.update(requests=0, request_acknowledgements=0, helper_wakeups=0,
                         helper_syscalls=0, syscall_fires=0, syscall_outputs=0,
                         request_to_frontier_samples_ns=[], max_request_to_frontier_ns=0)
                c["sites"][1].update(produced=0, successes=0, delivered=0)
                r["boundary_fires"] -= 2
                r["boundary_outputs"] -= 2
                self.assertIn(f"syscall_request: CPU {c['cpu']}: no request reached starved CPU",
                              qual.evaluate_runs(broken, cpus, "hog_data", True)["errors"])
        self.assertIn("syscall_request: hog_data requires at least 2 participating CPUs",
                      qual.evaluate_runs(quartet(), 1, "hog_data", True)["errors"])

    def test_hog_data_requested_latency_fields_and_pooled_nearest_rank(self):
        runs = hog_data_runs(4)
        r = runs["syscall_request"]
        # 100 starved samples: 99 at 2 ms and one at 9 ms. P99 is 2 ms;
        # pooling per-CPU maxima or percentiles instead would give 9 ms.
        for c, samples in zip(r["per_cpu"], ([7, 8], [], [2_000_000]*99, [9_000_000])):
            n = len(samples)
            c.update(request_to_frontier_samples_ns=samples, max_request_to_frontier_ns=max(samples, default=0),
                     requests=n, request_acknowledgements=n, helper_wakeups=n, helper_syscalls=n,
                     syscall_fires=n, syscall_outputs=n)
            c["sites"][1].update(produced=n, successes=n, delivered=n)
        r["boundary_fires"] = r["boundary_outputs"] = 102
        # 300 released DATA ages: 297 at 40 ms, three at 100 ms.
        for c in r["per_cpu"][:2]:
            c["sites"][0].update(produced=150, successes=150, delivered=150)
        r.update(hits=300, attempts=402)
        r["held_data"].update(arrived=300, released=300, held_age_samples=300,
                             max_held_data_age_ns=100_000_000, p99_held_data_age_ns=40_000_000)
        result = qual.evaluate_runs(runs, 4, "hog_data", True)
        self.assertEqual(result["status"], "PASS", result["errors"])
        latency = result["requested_latency"]
        self.assertEqual(latency["held_data_age_ns"], {"samples": 300, "max": 100_000_000, "p99": 40_000_000})
        self.assertEqual(latency["request_to_frontier_latency_ns"], {
            "starved": {"cpus": [8, 11], "samples": 100, "max": 9_000_000, "p99": 2_000_000},
            "other": {"cpus": [2, 5], "samples": 2, "max": 8, "p99": 8}})
        self.assertEqual(latency["requests_per_starved_cpu"], {"8": 99, "11": 1})
        self.assertEqual(qual.latency_summary(list(range(1, 102))), {"samples": 101, "max": 101, "p99": 100})
        r["held_data"]["max_held_data_age_ns"] += 1
        result = qual.evaluate_runs(runs, 4, "hog_data", True)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("syscall_request: held DATA age exceeds 100 ms", result["errors"])
        self.assertEqual(result["requested_latency"]["held_data_age_ns"]["max"], 100_000_001)

    def test_new_latency_evidence_fails_closed_and_empty_groups_are_null(self):
        for field in ("held_age_samples", "p99_held_data_age_ns"):
            for value in (None, -1, True, 1.5, 100_000_001):
                runs = hog_data_runs()
                runs["syscall_request"]["held_data"][field] = value
                self.assertEqual(qual.evaluate_runs(runs, 2, "hog_data", True)["status"], "FAIL", (field, value))
            runs = hog_data_runs()
            del runs["syscall_request"]["held_data"][field]
            self.assertEqual(qual.evaluate_runs(runs, 2, "hog_data", True)["status"], "FAIL", field)
        for index in (0, 1):
            for value in (None, [], [1], [-1, 1_000_000], [True, 1_000_000], [0, 999_999]):
                runs = hog_data_runs()
                runs["syscall_request"]["per_cpu"][index]["request_to_frontier_samples_ns"] = value
                errors = qual.evaluate_runs(runs, 2, "hog_data", True)["errors"]
                self.assertIn(f"syscall_request: CPU {2 + 3*index}: missing or inconsistent request latency samples", errors)
            runs = hog_data_runs()
            del runs["syscall_request"]["per_cpu"][index]["request_to_frontier_samples_ns"]
            self.assertEqual(qual.evaluate_runs(runs, 2, "hog_data", True)["status"], "FAIL")
        result = qual.evaluate_runs(quartet("idle"), 1, "idle", True)["requested_latency"]
        self.assertEqual(result["held_data_age_ns"], {"samples": 0, "max": None, "p99": None})
        self.assertEqual(result["request_to_frontier_latency_ns"]["starved"],
                         {"cpus": [], "samples": 0, "max": None, "p99": None})
        self.assertEqual(result["requests_per_starved_cpu"], {})

    def test_hog_data_one_cpu_skip_is_persisted_and_printed(self):
        args = qual.parse_args(["--out", "/tmp/unused", "--cases", "hog_data"])
        for available in (1, 3, 4):
            plan = qual.plan_cells(args, available)
            self.assertTrue(all(c["cpus"] >= 2 for c in plan["cells"]))
            self.assertEqual([c["cpus"] for c in plan["cells"]], [4]*3 + ([] if available == 1 else [available]*3))
            self.assertEqual([s["population"] for s in plan["skipped"]], ["1", "all"] if available == 1 else ["1"])
            self.assertTrue(all("at least 2" in s["reason"] for s in plan["skipped"]))

    def test_period_grid_runs_all_six_cases_and_each_request_treatment(self):
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
                     mock.patch.object(qual.os, "sched_getaffinity", return_value={0, 1, 2, 3}, create=True), \
                     mock.patch.object(qual, "capture", return_value={"exit": 0}), \
                     mock.patch.object(qual, "metadata", return_value={"allowed_cpus": [0, 1, 2, 3], "irq_time_accounting": True}), \
                     mock.patch.object(qual, "run_one", side_effect=run_one), \
                     mock.patch.object(qual, "control_errors", return_value=[]), \
                     mock.patch.object(qual, "evaluate_runs", return_value={"status": "PASS"}), \
                     mock.patch("builtins.print") as printed:
                    self.assertEqual(qual.main(), 0)
                cells = json.loads((out / "summary.json").read_text())["cells"]
                self.assertIn("Skipped cpu-1-hog_data: hog_data requires at least 2 participating CPUs",
                              [c.args[0] for c in printed.call_args_list])
                self.assertEqual(json.loads((out / "plan.json").read_text())["skipped"], [
                    {"population": "1", "cpus": 1, "case": "hog_data",
                     "reason": "hog_data requires at least 2 participating CPUs"}])
                self.assertEqual(len(cells), 17 * len(periods))
                self.assertEqual(set(qual.CASES), {"idle", "one", "all", "migrate", "hog", "hog_data"})
                for population in ("1", "4", "all"):
                    for case in qual.CASES:
                        if case == "hog_data" and population == "1":
                            continue
                        for period in periods:
                            prefix = f"cpu-{population}-{case}-p{period}-r1"
                            for arm in qual.ARMS:
                                args = next(args for label, args in calls if label == prefix + "-" + arm)
                                self.assertEqual(args[args.index("--case") + 1], case)
                                if arm in qual.BOUNDARY_ARMS:
                                    self.assertEqual(int(args[args.index("--period-ns") + 1]), period)
                                self.assertEqual("--syscall-request" in args, arm == "syscall_request")
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

    def test_request_wrong_treatment_and_run_capture(self):
        r = report()
        r.update(transport="perf", case="all", cpus=1, duration=30,
                 period_ns=10_000_000, buffer_bytes_total=67108864, boundary_source="syscall_request")
        args = ["--transport", "perf", "--case", "all", "--cpus", "1", "--duration", "30",
                "--period-ns", "10000000", "--bytes", "67108864", "--syscall-request"]
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
            actual = qual.run_one(Path(tmp), "request", args)
            self.assertEqual(actual["non_bench_process_cpu"]["bench_pid"], 20)
            self.assertEqual(actual["non_bench_process_cpu"]["top3"][0]["cpu_ticks_delta"], 1)
            raw = json.loads((Path(tmp) / "request.raw.json").read_text())
            self.assertEqual(raw["non_bench_process_cpu"], actual["non_bench_process_cpu"])
            self.assertEqual(raw["command"][-1], "--syscall-request")

    def test_frontier_gate_keeps_boundary_only_failures_visible(self):
        for arm in ("perf_boundary", "syscall_boundary"):
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
        for arm in ("perf_boundary", "syscall_boundary"):
            broken = copy.deepcopy(runs)
            broken[arm]["per_cpu"][0]["max_frontier_gap_ns"] += 1
            self.assertIn(arm + ": CPU 0: frontier gap exceeds 100 ms",
                          qual.evaluate_runs(broken, 1, "hog", True)["errors"])

    def test_request_parks_without_data_and_can_release_from_data_only_frontiers(self):
        for case in ("idle", "hog"):
            runs = quartet(case)
            result = qual.evaluate_runs(runs, 1, case, True)
            self.assertEqual(result["status"], "PASS", result["errors"])
            self.assertIsNone(result['timing_diagnostics']['syscall_request']['per_cpu']['0']['request_to_frontier_latency_ns'])
            c = result["boundary_cadence_per_cpu"]["syscall_request"]["0"]
            self.assertEqual(c, {"fires": 0, "expected": 0, "fires_per_expected": None,
                                 "helper_wakeups": 0, "helper_syscalls": 0})
        runs = quartet()
        r = runs["syscall_request"]
        r.update(boundary_fires=0, boundary_outputs=0)
        cpu = r["per_cpu"][0]
        cpu.update(syscall_fires=0, syscall_outputs=0, helper_syscalls=0, helper_wakeups=0,
                   requests=0, request_acknowledgements=0, max_request_to_frontier_ns=0, request_to_frontier_samples_ns=[])
        cpu["sites"][1].update(produced=0, successes=0, delivered=0)
        self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "PASS")
        cpu["helper_syscalls"] = 1
        self.assertIn("syscall_request: CPU 0: syscall helper/counter mismatch",
                      qual.evaluate_runs(runs, 1, "all", True)["errors"])

    def test_every_boundary_arm_reconciles_consumer_delivery(self):
        for arm in ("perf_boundary", "syscall_boundary"):
            runs = quartet()
            r = runs[arm]
            self.assertEqual(r["boundary_fires"], r["boundary_outputs"])
            r["per_cpu"][0]["sites"][1].update(delivered=1, loss=1, seq_gaps=1)
            self.assertIn(arm + ": CPU 0: boundary outputs != consumer-delivered BOUNDARY",
                          qual.evaluate_runs(runs, 1, "all", True)["errors"])

    def test_request_preserves_syscall_safety_on_each_cpu(self):
        runs = quartet()
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, helper_tid=124 if second["helper_tid"] else 0)
            r["per_cpu"].append(second)
            if isinstance(r["held_data"], dict):
                r["held_data"]["arrived"] *= 2
                r["held_data"]["released"] *= 2
                r["held_data"]["held_age_samples"] *= 2
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("hits", "attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        self.assertEqual(qual.evaluate_runs(runs, 2, "all", True)["status"], "PASS")
        for field, value, message in (
                ("wrong_cpu_events", 1, "wrong-CPU syscall boundary event"),
                ("helper_tid", 0, "syscall helper/counter mismatch"),
                ("syscall_fires", 0, "syscall helper/counter mismatch"),
                ("syscall_outputs", 0, "syscall helper/counter mismatch"),
                ("max_request_to_frontier_ns", -1, "invalid max_request_to_frontier_ns"),
                ("seal_violations", 1, "seal violation or timestamp regression"),
                ("timestamp_regressions", 1, "seal violation or timestamp regression")):
            broken = copy.deepcopy(runs)
            broken["syscall_request"]["per_cpu"][1][field] = value
            self.assertTrue(any(message in e for e in qual.evaluate_runs(broken, 2, "all", True)["errors"]), field)
        for field, value, message in (
                ("wrong_cpu_events", 1, "wrong-CPU syscall boundary event"),
                ("boundary_fires", 5, "syscall boundary fires != outputs"),
                ("boundary_outputs", 5, "syscall boundary fires != outputs"),
                ("lost_notifications", 1, "positive PERF_RECORD_LOST")):
            broken = copy.deepcopy(runs)
            broken["syscall_request"][field] = value
            self.assertTrue(any(message in e for e in qual.evaluate_runs(broken, 2, "all", True)["errors"]), field)
        broken = copy.deepcopy(runs)
        broken["syscall_request"].update(boundary_fires=6, boundary_outputs=6)
        self.assertIn("syscall_request: syscall boundary aggregate/per-CPU counter mismatch",
                      qual.evaluate_runs(broken, 2, "all", True)["errors"])

        runs = quartet("hog")
        for r in runs.values():
            second = copy.deepcopy(r["per_cpu"][0])
            second.update(cpu=1, hog_tid=322, helper_tid=124 if second["helper_tid"] else 0)
            r["per_cpu"].append(second)
            if isinstance(r["held_data"], dict):
                r["held_data"]["arrived"] *= 2
                r["held_data"]["released"] *= 2
                r["held_data"]["held_age_samples"] *= 2
            set_busy(r, r["participating_busy_cpu_ns"])
            for field in ("attempts", "boundary_fires", "boundary_outputs"):
                r[field] *= 2
        self.assertEqual(qual.evaluate_runs(runs, 2, "hog", True)["status"], "PASS")
        runs["syscall_request"]["per_cpu"][1]["hog_iterations"] = 0
        self.assertIn("syscall_request: CPU 1: untraced hog did not run",
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
                    runs["syscall_request"]["per_cpu"][0][field] = value
                    self.assertEqual(qual.evaluate_runs(runs, 1, "all", True)["status"], "FAIL")
            runs = quartet()
            del runs["syscall_request"]["per_cpu"][0][field]
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

    def test_request_uses_schedstat_and_helper_one_percent_limits(self):
        for irq in (True, False):
            runs = quartet("idle")
            cpu = runs["syscall_request"]["per_cpu"][0]
            cpu["schedstat_running_ns"].update(end=10_000_150, delta=10_000_050)
            cpu["helper_cpu_ns"] = 10_000_000
            result = qual.evaluate_runs(runs, 1, "idle", irq)
            gate = result["boundary_cpu_gates"]["syscall_request"]
            self.assertEqual(gate["status"], "PASS")
            self.assertAlmostEqual(gate["increment_core_fraction_per_cpu"]["0"], 0.01)
            self.assertEqual(gate["helper_core_fraction_per_cpu"], {"0": 0.01})
            cpu["helper_cpu_ns"] += 1
            self.assertIn("syscall_request: helper CPU exceeds 1% on a CPU",
                          qual.evaluate_runs(runs, 1, "idle", irq)["errors"])
            cpu["helper_cpu_ns"] -= 1
            cpu["schedstat_running_ns"]["end"] += 1
            cpu["schedstat_running_ns"]["delta"] += 1
            self.assertIn("syscall_request: schedstat idle increment exceeds 1% on a CPU",
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
            # Reuse the hand-counted streams, with two CPUs for hog_data.
            # One-CPU migration is pinned; idle has boundaries but zero DATA.
            runs = quartet(case)
            self.assertEqual(qual.evaluate_runs(runs, len(runs["perf"]["per_cpu"]), case, True)["status"], "PASS", case)
            for field in ("max_frontier_gap_ns", "max_frontier_stamp_gap_ns"):
                good = copy.deepcopy(runs)
                good["syscall_boundary"]["per_cpu"][0][field] = 100_000_000
                self.assertEqual(qual.evaluate_runs(good, len(runs["perf"]["per_cpu"]), case, True)["status"], "PASS")
                good["syscall_boundary"]["per_cpu"][0][field] += 1
                result = qual.evaluate_runs(good, len(runs["perf"]["per_cpu"]), case, True)
                self.assertEqual(result["status"], "FAIL")
                cpu_id = runs["syscall_boundary"]["per_cpu"][0]["cpu"]
                self.assertIn(f"syscall_boundary: CPU {cpu_id}: frontier gap exceeds 100 ms", result["errors"])
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0]["sites"][1].update(
                successes=1, failures=1, delivered=1, loss=1, seq_gaps=1)
            result = qual.evaluate_runs(broken, len(runs["perf"]["per_cpu"]), case, True)
            cpu_id = runs["syscall_boundary"]["per_cpu"][0]["cpu"]
            self.assertIn(f"syscall_boundary: CPU {cpu_id}: boundary fires != outputs", result["errors"])
            broken = copy.deepcopy(runs)
            broken["syscall_boundary"]["per_cpu"][0]["sites"][1].update(
                produced=0, successes=0, delivered=0)
            self.assertTrue(any("boundary did not fire" in e for e in
                                qual.evaluate_runs(broken, len(runs["perf"]["per_cpu"]), case, True)["errors"]))

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
                    self.assertEqual(qual.evaluate_runs(runs, len(runs["perf"]["per_cpu"]), case, True)["status"], "PASS")
                    (r["per_cpu"][0] if local else r)[field] = 1
                    self.assertIn("syscall_boundary: seal violation or timestamp regression",
                                  qual.evaluate_runs(runs, len(runs["perf"]["per_cpu"]), case, True)["errors"])
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
            if isinstance(r["held_data"], dict):
                r["held_data"]["arrived"] *= 2
                r["held_data"]["released"] *= 2
                r["held_data"]["held_age_samples"] *= 2
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
            if isinstance(r["held_data"], dict):
                r["held_data"]["arrived"] *= 2
                r["held_data"]["released"] *= 2
                r["held_data"]["held_age_samples"] *= 2
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

    def test_held_age_is_primary_and_old_frontier_failures_stay_visible(self):
        runs = quartet()
        r = runs['syscall_request']
        c = r['per_cpu'][0]
        c.update(max_frontier_gap_ns=2_889_000_000, max_frontier_stamp_gap_ns=996_000_000,
                 max_boundary_gap_ns=2_889_000_000, max_boundary_stamp_gap_ns=996_000_000)
        r['held_data']['max_held_data_age_ns'] = 100_000_000
        r['held_data']['p99_held_data_age_ns'] = 100_000_000
        result = qual.evaluate_runs(runs, 1, 'all', True)
        self.assertEqual(result['status'], 'PASS', result['errors'])
        self.assertEqual(result['held_data_gate']['limit_ns'], 100_000_000)
        diag = result['timing_diagnostics']['syscall_request']
        self.assertEqual(diag['consumer_drain_gap_ms'], 10)
        self.assertEqual(diag['per_cpu']['0'], {
            'request_to_frontier_latency_ns': 1_000_000, 'request_outstanding_age_ns': 0,
            'producer_stamp_gap_ns': 996_000_000, 'contract_frontier_gap_ns': 2_889_000_000,
            'boundary_only_gap_ns': 2_889_000_000, 'boundary_only_stamp_gap_ns': 996_000_000})
        r['held_data']['max_held_data_age_ns'] += 1
        self.assertIn('syscall_request: held DATA age exceeds 100 ms',
                      qual.evaluate_runs(runs, 1, 'all', True)['errors'])
        r['held_data']['max_held_data_age_ns'] -= 1
        runs['syscall_boundary']['per_cpu'][0]['max_frontier_gap_ns'] += 1
        self.assertIn('syscall_boundary: CPU 0: frontier gap exceeds 100 ms',
                      qual.evaluate_runs(runs, 1, 'all', True)['errors'])

    def test_held_data_all_fields_fail_closed_and_unreleased_data_never_passes(self):
        base = quartet()['syscall_request']
        for field in base['held_data']:
            broken = copy.deepcopy(base)
            del broken['held_data'][field]
            self.assertTrue(qual.held_data_errors(broken), field)
            for value in (None, -1, float('nan')):
                broken = copy.deepcopy(base)
                broken['held_data'][field] = value
                self.assertTrue(qual.held_data_errors(broken), (field, value))
        for field, value in (('arrived', 4), ('released', 2), ('queue_depth', 1),
                             ('queue_max_depth', 0), ('queue_growth_records', 1),
                             ('max_queue_growth_per_drain', 2), ('queue_growth_records_per_second', 1)):
            broken = copy.deepcopy(base)
            broken['held_data'][field] = value
            self.assertIn('held DATA/queue accounting mismatch', qual.held_data_errors(broken), field)
        h = base['held_data']
        h.update(released=2, held_age_samples=2, queue_depth=1, queue_growth_records=1, queue_growth_records_per_second=1,
                 oldest_pending_age_ns=100_000_001)
        errors = qual.held_data_errors(base)
        self.assertIn('held DATA age exceeds 100 ms', errors)
        self.assertIn('consumed DATA remains held at end of cell', errors)
        for value in ('UNMEASURED', None, {}):
            base['held_data'] = value
            self.assertTrue(qual.held_data_errors(base))
            runs = quartet()
            runs['syscall_request'] = copy.deepcopy(base)
            result = qual.evaluate_runs(runs, 1, 'all', True)
            self.assertEqual(result['held_data_gate']['status'], 'FAIL')
            self.assertEqual(result['status'], 'FAIL')

    def test_request_counters_and_diagnostics_cannot_be_missing_or_coalescing_broken(self):
        fields = ('requests', 'request_acknowledgements', 'request_outstanding',
                  'max_request_to_frontier_ns', 'request_outstanding_age_ns', 'phase_offset_ns',
                  'schedstat_running_ns')
        for field in fields:
            runs = quartet()
            del runs['syscall_request']['per_cpu'][0][field]
            self.assertEqual(qual.evaluate_runs(runs, 1, 'all', True)['status'], 'FAIL', field)
        for field, value in (('requests', 3), ('request_acknowledgements', 3), ('request_outstanding', True)):
            runs = quartet()
            runs['syscall_request']['per_cpu'][0][field] = value
            self.assertIn('syscall_request: CPU 0: request/coalescing counter mismatch',
                          qual.evaluate_runs(runs, 1, 'all', True)['errors'])
        for case in ('idle', 'hog'):
            runs = quartet(case)
            r = runs['syscall_request']
            r['per_cpu'][0].update(requests=1, request_acknowledgements=1,
                                   helper_syscalls=1, helper_wakeups=1, syscall_fires=1, syscall_outputs=1,
                                   max_request_to_frontier_ns=1_000_000, request_to_frontier_samples_ns=[1_000_000])
            r['per_cpu'][0]['sites'][1].update(produced=1, successes=1, delivered=1)
            r.update(boundary_fires=1, boundary_outputs=1)
            self.assertIn('syscall_request: request helper woke without staged DATA or coverage',
                          qual.evaluate_runs(runs, 1, case, True)['errors'])
            r['coverage_ms'] = 50
            self.assertEqual(qual.evaluate_runs(runs, 1, case, True)['status'], 'PASS')

    def test_schedstat_every_cpu_one_percent_not_an_average_or_ticks(self):
        runs = quartet('idle')
        for r in runs.values():
            second = copy.deepcopy(r['per_cpu'][0])
            second.update(cpu=3, helper_tid=124 if second['helper_tid'] else 0)
            r['per_cpu'].append(second)
            r['online_cpus'] = 2
            set_busy(r, r['participating_busy_cpu_ns'])
            for field in ('attempts', 'boundary_fires', 'boundary_outputs'):
                r[field] *= 2
        for arm in qual.BOUNDARY_ARMS:
            r = runs[arm]
            c = r['per_cpu'][1]
            c['schedstat_running_ns'].update(end=10_000_150, delta=10_000_050)
            result = qual.evaluate_runs(runs, 2, 'idle', True)
            gate = result['schedstat_idle_cpu_gates'][arm]
            self.assertEqual(gate['status'], 'PASS')
            self.assertAlmostEqual(gate['increment_core_fraction_per_cpu']['3'], .01)
            self.assertEqual(gate['increment_core_fraction_per_cpu']['0'], 0)
            c['schedstat_running_ns']['end'] += 1
            c['schedstat_running_ns']['delta'] += 1
            result = qual.evaluate_runs(runs, 2, 'idle', True)
            self.assertIn(arm + ': schedstat idle increment exceeds 1% on a CPU', result['errors'])
            c['schedstat_running_ns'].update(end=150, delta=50)
        # Nanosecond intervals, rather than rounded seconds or tick fields.
        baseline, boundary = report(), report()
        baseline['schedstat_elapsed_ns'] = 2_000_000_000
        boundary['schedstat_elapsed_ns'] = 4_000_000_000
        baseline['per_cpu'][0]['schedstat_running_ns'].update(end=200_000_100, delta=200_000_000)
        boundary['per_cpu'][0]['schedstat_running_ns'].update(end=440_000_100, delta=440_000_000)
        self.assertAlmostEqual(qual.schedstat_increment(boundary, baseline)['0'], .01)
        boundary['per_cpu'][0]['schedstat_running_ns']['delta'] += 4
        self.assertGreater(qual.schedstat_increment(boundary, baseline)['0'], .01)

    def test_precise_limits_do_not_round_an_exact_one_percent_into_failure(self):
        runs = quartet('idle')
        for arm in runs:
            runs[arm]['per_cpu'][0]['schedstat_running_ns'].update(start=0, end=7_000_000, delta=7_000_000)
        r = runs['syscall_request']
        r['per_cpu'][0]['schedstat_running_ns'].update(end=17_000_000, delta=17_000_000)
        self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'PASS')
        r['per_cpu'][0]['schedstat_running_ns'].update(end=17_000_001, delta=17_000_001)
        self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'FAIL')
        r['schedstat_elapsed_ns'] = 3_000_000_000
        r['per_cpu'][0]['schedstat_running_ns'].update(end=51_000_000, delta=51_000_000)
        self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'PASS')
        r['per_cpu'][0]['schedstat_running_ns'].update(end=51_000_001, delta=51_000_001)
        self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'FAIL')

    def test_schedstat_absence_is_unmeasured_and_malformed_is_failure(self):
        for arm in ('perf',) + qual.BOUNDARY_ARMS:
            runs = quartet('idle')
            c = runs[arm]['per_cpu'][0]
            c['schedstat_running_ns'] = dict(status='UNMEASURED', start=None, end=None, delta=None)
            result = qual.evaluate_runs(runs, 1, 'idle', True)
            self.assertEqual(result['status'], 'VOID')
            self.assertEqual(result['schedstat_idle_cpu_gates']['syscall_request' if arm == 'perf' else arm]['status'],
                             'UNMEASURED')
            c['schedstat_running_ns']['delta'] = 0
            self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'FAIL')
        for field in ('status', 'start', 'end', 'delta'):
            for value in (None, -1, 'bad'):
                runs = quartet()
                runs['perf']['per_cpu'][0]['schedstat_running_ns'][field] = value
                self.assertEqual(qual.evaluate_runs(runs, 1, 'all', True)['status'], 'FAIL', (field, value))
        runs = quartet()
        runs['perf']['per_cpu'][0]['schedstat_running_ns']['delta'] += 1
        self.assertIn('perf: invalid schedstat running nanoseconds', qual.evaluate_runs(runs, 1, 'all', True)['errors'])
        for field in ('cpu_accounting_elapsed_ns', 'schedstat_elapsed_ns', 'online_cpus',
                      'max_drain_gap_ms', 'request_after_ms', 'coverage_ms', 'phase_jitter'):
            runs = quartet()
            del runs['perf'][field]
            self.assertEqual(qual.evaluate_runs(runs, 1, 'all', True)['status'], 'FAIL', field)

    def test_proc_stat_closure_half_percent_and_both_signs(self):
        r = report()
        for ticks, status in ((1_000_000_000, 'OK'), (1_005_000_000, 'OK'), (995_000_000, 'OK'),
                              (1_005_000_001, 'SUSPECT'), (994_999_999, 'SUSPECT')):
            set_busy(r, ticks)
            closure = qual.accounting_closure(r)
            for row in (closure['participating'], closure['whole_box'], closure['per_cpu']['0']):
                self.assertEqual(row['state_sum_ns'], ticks)
                self.assertEqual(row['cpus_x_elapsed_ns'], 1_000_000_000)
                self.assertEqual(row['status'], status)
        runs = quartet('idle')
        # Keep each CPU accounting field coherent while reproducing a >0.5% closure error.
        set_busy(runs['perf'], 5_000_000)
        set_busy(runs['syscall_boundary'], 5_000_000)
        result = qual.evaluate_runs(runs, 1, 'idle', True)
        self.assertEqual(result['cpu_accounting']['syscall_boundary']['proc_stat_accounting_closure']['whole_box']['status'],
                         'SUSPECT')
        self.assertIn('proc_stat_increment_core_fraction_per_participating_cpu', result)
        self.assertIn('schedstat_idle_cpu_gates', result)

    def test_options_defaults_filters_and_negative_controls(self):
        default = qual.parse_args(['--out', '/tmp/unused'])
        self.assertEqual((default.request_after_ms, default.coverage_ms, default.phase_jitter), (50, 0, False))
        filtered = qual.parse_args(['--out', '/tmp/unused', '--cases', 'one', 'idle', '--arms', 'perf', 'syscall_request',
                                    '--request-after-ms', '25', '--coverage-ms', '75', '--phase-jitter', '--repeats', '2'])
        plan = qual.plan_cells(filtered, 4)
        self.assertEqual(plan['planned_cell_count'], 12)
        self.assertEqual(plan['planned_arm_runs'], 24)
        self.assertEqual(plan['planned_control_runs'], 36)
        self.assertEqual(plan['estimated_wall_seconds'], 24*30 + 36*2 + 60*2)
        for cell in plan['cells']:
            self.assertIn(cell['case'], ('one', 'idle'))
            self.assertEqual(set(cell['arm_order']), {'perf', 'syscall_request'})
        command = qual.arm_command('syscall_request', [], 'one', 30, 10_000_000, filtered)
        for flag, value in (('--request-after-ms', '25'), ('--coverage-ms', '75')):
            self.assertEqual(command[command.index(flag)+1], value)
        self.assertIn('--phase-jitter', command)
        self.assertIn('--syscall-request', command)
        for flags in (['--cases', 'bogus'], ['--arms', 'syscall_demand'], ['--cases', 'idle', 'idle'],
                      ['--arms', 'perf', 'perf'], ['--request-after-ms', '-1'], ['--coverage-ms', '-1'],
                      ['--coverage-ms', str(2**64)], ['--request-after-ms', str(2**64)],
                      ['--crossover', '0'], ['--crossover', '-1'], ['--phase-jitter', 'yes'],
                      ['--crossover', '2', '--cases', 'idle'], ['--crossover', '2', '--arms', 'perf']):
            with mock.patch.object(sys, 'stderr'), self.assertRaises(SystemExit):
                qual.parse_args(['--out', '/tmp/unused'] + flags)

    def test_requested_options_and_jitter_are_checked(self):
        r = report()
        r.update(transport='perf', case='all', cpus=1, duration=30, buffer_bytes_total=67108864,
                 boundary_source='syscall_request', request_after_ms=25, coverage_ms=75, phase_jitter=True)
        args = ['--transport', 'perf', '--case', 'all', '--cpus', '1', '--duration', '30',
                '--period-ns', '10000000', '--bytes', '67108864', '--syscall-request',
                '--request-after-ms', '25', '--coverage-ms', '75', '--phase-jitter']
        self.assertEqual(qual.requested_errors(r, args, [0]), [])
        for field, value in (('request_after_ms', 50), ('coverage_ms', 0), ('phase_jitter', False)):
            broken = copy.deepcopy(r)
            broken[field] = value
            self.assertTrue(any(field in e for e in qual.requested_errors(broken, args, [0])))
        runs = quartet()
        r = runs['syscall_boundary']
        r['phase_jitter'] = True
        r['per_cpu'][0]['phase_offset_ns'] = 999_999
        self.assertEqual(qual.evaluate_runs(runs, 1, 'all', True)['status'], 'PASS')
        for jitter, offset in ((True, 1_000_000), (False, 1), (False, -1)):
            r['phase_jitter'] = jitter
            r['per_cpu'][0]['phase_offset_ns'] = offset
            self.assertEqual(qual.evaluate_runs(runs, 1, 'all', True)['status'], 'FAIL')

    def test_crossover_is_randomized_alternating_idle_all_cpus_and_exact_count(self):
        args = qual.parse_args(['--out', '/tmp/unused', '--crossover', '3', '--phase-jitter'])
        class Reverse:
            def shuffle(self, order):
                order.reverse()
        with mock.patch.object(qual.random, 'SystemRandom', return_value=Reverse()):
            plan = qual.plan_cells(args, 6)
        self.assertEqual(plan['planned_cell_count'], 3)
        self.assertEqual(plan['planned_arm_runs'], 6)
        flat = [a for c in plan['cells'] for a in c['arm_order']]
        self.assertEqual(flat, ['syscall_boundary', 'perf'] * 3)
        for c in plan['cells']:
            self.assertEqual((c['cpus'], c['case'], c['period_ns']), (6, 'idle', 10_000_000))
        # Opposite random start is observable in the persisted plan.
        with mock.patch.object(qual.random.SystemRandom, 'shuffle', return_value=None):
            other = qual.plan_cells(args, 6)
        self.assertEqual(other['cells'][0]['arm_order'], ['perf', 'syscall_boundary'])
        runs = {arm: r for arm, r in quartet('idle').items() if arm in ('perf', 'syscall_boundary')}
        result = qual.evaluate_runs(runs, 1, 'idle', True)
        self.assertEqual(result['status'], 'PASS')
        for arm in runs:
            self.assertIn('schedstat_running_ns_per_cpu', result['cpu_accounting'][arm])
            self.assertIn('proc_stat_accounting_closure', result['cpu_accounting'][arm])
        runs['syscall_boundary']['per_cpu'][0]['max_frontier_gap_ns'] += 1
        self.assertEqual(qual.evaluate_runs(runs, 1, 'idle', True)['status'], 'FAIL')

    def test_filter_missing_baseline_is_unmeasured_instead_of_crash_or_pass(self):
        r = quartet('idle')['syscall_request']
        result = qual.evaluate_runs({'syscall_request': r}, 1, 'idle', True)
        self.assertEqual(result['status'], 'VOID')
        self.assertEqual(result['boundary_cpu_gates']['syscall_request']['status'], 'UNMEASURED')
        self.assertIsNone(result['perf_shared_cpu_ns_per_attempt_ratio'])

    def test_runner_writes_plan_raw_and_parsed_evidence_and_rejects_bad_stdout(self):
        r = report()
        r.update(transport='perf', case='all', cpus=1, duration=30,
                 buffer_bytes_total=67108864, boundary_source='perf')
        args = ['--transport', 'perf', '--case', 'all', '--cpus', '1', '--duration', '30',
                '--period-ns', '10000000', '--bytes', '67108864']
        snapshot = {'records': {}, 'observed_ns': 1, 'unreadable': 0}
        for stdout, success in ((json.dumps(r), True), ('not JSON', False)):
            with tempfile.TemporaryDirectory() as tmp, \
                 mock.patch.object(qual, 'process_snapshot', return_value=snapshot), \
                 mock.patch.object(qual.os, 'sched_getaffinity', return_value={0}, create=True), \
                 mock.patch.object(qual.subprocess, 'Popen') as popen:
                process = popen.return_value.__enter__.return_value
                process.pid, process.returncode = 20, 0
                process.communicate.return_value = (stdout, 'diagnostic')
                if success:
                    actual = qual.run_one(Path(tmp), 'arm', args)
                    saved = json.loads((Path(tmp)/'arm.json').read_text())
                    self.assertEqual(actual, saved)
                    self.assertIn('proc_stat_accounting_closure', saved)
                    self.assertIn('schedstat_running_ns', saved['per_cpu'][0])
                else:
                    with self.assertRaises(ValueError):
                        qual.run_one(Path(tmp), 'arm', args)
                    self.assertFalse((Path(tmp)/'arm.json').exists())
                raw = json.loads((Path(tmp)/'arm.raw.json').read_text())
                self.assertEqual(raw['stdout'], stdout)
                self.assertEqual(raw['stderr'], 'diagnostic')
                self.assertEqual(raw['exit'], 0)
                self.assertEqual(raw['command'][-len(args):], args)

    def test_crossover_main_persists_each_run_both_accountings_and_startup_plan(self):
        snapshot = {'records': {}, 'observed_ns': 1, 'unreadable': 0}
        commands = []
        def launch(command, **unused):
            commands.append(command)
            def value(flag, default=None):
                return command[command.index(flag)+1] if flag in command else default
            case = value('--case')
            arm = 'syscall_boundary' if '--syscall-boundary' in command else 'perf_boundary' if '--period-ns' in command else 'perf'
            r = quartet(case)[arm]
            period = int(value('--period-ns', '0'))
            duration = int(value('--duration'))
            total = qual.os.sysconf('SC_PAGESIZE') if '--one-page' in command else int(value('--bytes'))
            r.update(transport='perf', case=case, cpus=1, duration=duration, period_ns=period,
                     boundary_source='syscall' if arm == 'syscall_boundary' else 'perf' if period else 'none',
                     pause_drain='--pause-drain' in command, phase_jitter='--phase-jitter' in command,
                     buffer_bytes_total=total, buffer_bytes=[total],
                     boundary_offset_ns=1_000_000 if '--mutant-boundary' in command else 0)
            r['clock_ticks'] = qual.os.sysconf('SC_CLK_TCK')
            set_busy(r, 0)
            r['per_cpu'][0]['boundary_expected'] = (duration*1_000_000_000+period-1)//period if period else 0
            process = mock.MagicMock()
            child = process.__enter__.return_value
            child.pid, child.returncode = 20, 0
            child.communicate.return_value = (json.dumps(r), '')
            return process
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)/'crossover'
            with mock.patch.object(sys, 'argv', ['qualify.py', '--out', str(out), '--seconds', '1',
                                                 '--crossover', '2', '--phase-jitter']), \
                 mock.patch.object(qual.platform, 'system', return_value='Linux'), \
                 mock.patch.object(qual.os, 'sched_getaffinity', return_value={0}, create=True), \
                 mock.patch.object(qual, 'capture', return_value={'exit': 0}), \
                 mock.patch.object(qual, 'metadata', return_value={'allowed_cpus': [0], 'irq_time_accounting': True}), \
                 mock.patch.object(qual, 'control_errors', return_value=[]), \
                 mock.patch.object(qual, 'process_snapshot', return_value=snapshot), \
                 mock.patch.object(qual.subprocess, 'Popen', side_effect=launch), \
                 mock.patch.object(qual.random.SystemRandom, 'shuffle', return_value=None), \
                 mock.patch('builtins.print') as printed:
                status = qual.main()
                self.assertEqual(status, 0, [json.loads(p.read_text())['errors'] for p in out.glob('*.summary.json')])
            self.assertIn('Planned cells: 2; arm runs: 4; estimated wall time:', printed.call_args_list[0].args[0])
            plan = json.loads((out/'plan.json').read_text())
            self.assertEqual([c['arm_order'] for c in plan['cells']], [['perf', 'syscall_boundary']]*2)
            for repeat in (1, 2):
                for arm in ('perf', 'syscall_boundary'):
                    label = f'crossover-r{repeat}-{arm}'
                    self.assertTrue((out/(label+'.raw.json')).is_file())
                    run = json.loads((out/(label+'.json')).read_text())
                    self.assertIn('proc_stat_accounting_closure', run)
                    self.assertIn('schedstat_running_ns', run['per_cpu'][0])
                    self.assertEqual(run['phase_jitter'], True)
                    self.assertEqual((run['case'], run['cpus']), ('idle', 1))
                summary = json.loads((out/f'crossover-r{repeat}.summary.json').read_text())
                self.assertEqual(summary['status'], 'PASS', summary['errors'])
                self.assertIn('schedstat_idle_cpu_gates', summary)
                self.assertIn('proc_stat_increment_core_fraction_per_participating_cpu', summary)
            self.assertFalse(json.loads((out/'summary.json').read_text())['qualification_claim'])
        measured = [c for c in commands if c[c.index('--duration')+1] == '1']
        self.assertEqual(['--syscall-boundary' in c for c in measured], [False, True, False, True])

    def test_portable_request_staging_and_accounting_mutants_reach_assertions(self):
        import shutil
        import subprocess
        source = Path(__file__).parent/'bench_nmi_transport'
        mutants = (
            ('staging.h', 's->count - s->count / 100 - 1', 's->count - 1', 'transport_p99(&s.held_ages) == 30'),
            ('staging.h', 's->count - s->count / 100 - 1', 's->count - s->count / 100', 'transport_p99(&samples) == 99'),
            ('staging.h', '.stamp < w', '.stamp <= w', 's.released == 0 && s.depth == 2'),
            ('staging.h', 'if (outstanding)', 'if (outstanding && false)', '!transport_request_due(true'),
            ('staging.h', 'frontier == minimum', 'frontier <= minimum + 1', '!transport_request_due(false, 11'),
            ('staging.h', 'age >= after', 'age + 1000000 >= after', '!transport_request_due(false, 10'),
            ('staging.h', 'return coverage &&', 'return true &&', '!transport_request_due(false, 0, 0, 0'),
            ('staging.h', 'now - q->records[q->head].arrival', 'now - q->records[q->head].stamp', 's.max_held_age_ns == 20'),
            ('cpu_accounting.h', 'snapshot[cpu] = (uint64_t)v[6]', 'snapshot[cpu] = (uint64_t)v[7]', 'running[0] == 7000000001'),
            ('request.h', 'pthread_cond_wait(&c->condition, &c->mutex);', 'c->wake = true;', 'atomic_load_explicit(&t.wakes, memory_order_acquire) == 0'),
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for path in list(source.glob('*.h')) + [source/'test_reader.c']:
                shutil.copyfile(path, root/path.name)
            command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', str(root/'test_reader.c'), '-o', str(root/'reader')]
            subprocess.run(command, check=True, capture_output=True)
            passed = subprocess.run([str(root/'reader')], cwd=root, capture_output=True, text=True, timeout=10)
            self.assertEqual(passed.returncode, 0, passed.stderr)
            for filename, before, after, assertion in mutants:
                with self.subTest(mutant=before):
                    path = root/filename
                    original = path.read_text()
                    self.assertIn(before, original)
                    path.write_text(original.replace(before, after))
                    try:
                        build = subprocess.run(command, capture_output=True, text=True)
                        self.assertEqual(build.returncode, 0, build.stderr)
                        broken = subprocess.run([str(root/'reader')], cwd=root, capture_output=True, text=True, timeout=10)
                        self.assertNotEqual(broken.returncode, 0)
                        self.assertIn(assertion, broken.stderr)
                    finally:
                        path.write_text(original)


if __name__ == "__main__":
    unittest.main()
