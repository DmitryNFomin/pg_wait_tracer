#!/usr/bin/env python3
"""Verdict refusals exercised through the production aggregators and launcher.

No browser, SSH or capture: the pass walker receives recorded tab measurements,
and collect_results receives a complete local fixture through a fake rsync.
"""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

import demo_rehearsal as dr
import demo_rehearsal_lib as demo
import ui_live_smoke_lib as ui

ROOT = Path(__file__).resolve().parents[1]


def clean_tab(**changes):
    kwargs = dict(tab_id="queries", rendered_ok=True, rendered_detail="rows",
                  ticks_observed=6, console_errors=[], blink_ratio=0.0,
                  color_violations=[], leak_before={"charts": 0, "uplots": 0, "pending": 0},
                  leak_after={"charts": 0, "uplots": 0, "pending": 0}, artifacts={},
                  blink_pair_offsets_ms=[200] * 6, ttfp_ms=3000)
    kwargs.update(changes)
    return ui.build_tab_result(**kwargs)


def attempt(tab=None, checks=None):
    return demo.build_demo_summary(
        [{"pass": "early", "tabs": {"queries": tab or clean_tab()}}],
        checks or {"fixture": {"ok": True}})


class VerdictRefusals(unittest.TestCase):
    def test_first_paint_boundary_and_per_tab_failure_detail(self):
        self.assertTrue(attempt()["ok"])
        bad = clean_tab(ttfp_ms=3001)
        self.assertFalse(attempt(bad)["ok"])
        detail = bad["first_paint"]["detail"]
        self.assertIn("queries", detail)
        self.assertIn("3001", detail)
        self.assertEqual(bad["first_paint"]["bound_ms"], 3000)

    def test_first_paint_absence_refuses(self):
        for value in (None, float("nan"), float("inf")):
            with self.subTest(value=value):
                self.assertFalse(attempt(clean_tab(ttfp_ms=value))["ok"])

    def test_zero_attempted_ticks_refuses(self):
        bad = clean_tab(blink_pair_offsets_ms=[])
        self.assertFalse(bad["no_blink"]["measured"]["ok"])
        self.assertFalse(attempt(bad)["ok"])
        half = clean_tab(blink_not_measured=[1, 2, 3])
        self.assertTrue(attempt(half)["ok"])
        self.assertFalse(attempt(clean_tab(blink_not_measured=[1, 2, 3, 4]))["ok"])

    def test_degraded_warning_refuses_attempt(self):
        for marker in ("fallback", "LEGACY gap-inference", "unavailable"):
            with self.subTest(marker=marker):
                ok, errors, warnings = demo.daemon_log_clean(f"WARN: {marker}\n")
                result = attempt(checks={"daemon_log_clean": {
                    "ok": ok, "errors": errors, "degraded_warnings": warnings}})
                self.assertFalse(result["ok"])
                self.assertEqual(len(warnings), 1)
        self.assertTrue(demo.daemon_log_clean("WARN: target effective CPU capacity unknown\n")[0])

    def test_offcpu_absence_refuses_each_sample_not_measured_zero(self):
        for measured in (True, False):
            sample = demo.evaluate_time_model_window(
                [{"name": "CPU (running)", "ms": 1000, "indent": 1}],
                db_time_ms=1000, cpu_clamped_ms=0, offcpu_ms=0,
                has_measured_cpu=measured, used_raw_path=True)
            check = demo.build_demo_conservation_check([sample], {"ok": True})
            self.assertEqual(attempt(checks={"time_model_conserves": check})["ok"], measured)
            self.assertEqual(sample["offcpu"]["available"], measured)
            self.assertEqual(sample["offcpu"]["offcpu_ms"], 0 if measured else None)
            self.assertEqual(sample["offcpu"]["pct"], 0 if measured else None)

    def test_query_never_fabricates_an_absent_offcpu_measurement(self):
        base = {"rows": [{"name": "CPU (running)", "ms": 1000, "indent": 1}],
                "db_time_ms": 1000, "cpu_clamped_ms": 0, "categories": []}
        for response in (dict(base, has_measured_cpu=False, offcpu_ms=0),
                         dict(base, has_measured_cpu=True)):
            server = type("Server", (), {"query": lambda *a, **kw: response})()
            result = dr._query_time_model_window(server, 0, 1, "fixture", True)
            self.assertFalse(result["ok"])
            self.assertIsNone(result["offcpu_ms"])
            self.assertIsNone(result["offcpu"]["pct"])

    def test_missing_sample_offcpu_availability_refuses(self):
        for offcpu in ({}, {"available": False}, {"available": None}):
            sample = {"ok": True, "offset_s": 300, "offcpu": offcpu}
            good = {"ok": True, "offset_s": 0, "offcpu": {"available": True}}
            check = demo.build_demo_conservation_check([good, sample], {"ok": True})
            self.assertFalse(attempt(checks={"time_model_conserves": check})["ok"])
            self.assertEqual(check["failed_offsets_s"], [300])

    def walk(self, first_offsets, other_tabs=None):
        tabs = {}
        for tab_id, offsets in dict(queries=first_offsets, **(other_tabs or {})).items():
            ticks = [{"target_offsets_ms": list(ui.SWEEP_OFFSETS_MS),
                      "achieved_offsets_ms": [offset, 600, 1100, 1600, 2100]}
                     for offset in offsets]
            tabs[tab_id] = {"ok": True, "ticks_observed": 6, "rendered": {"ok": True},
                            "no_blink": {"ok": True}, "blink_sweep": {"ticks": ticks}}
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(dr.lib, "TABS", list(tabs)), \
                patch.object(dr.live_smoke, "run_tab",
                             side_effect=lambda browser, tab_id, *a, **kw: tabs[tab_id]), \
                patch.object(dr.live_smoke, "_assert_workload_alive"), \
                patch.object(dr, "_assert_daemon_alive"), \
                contextlib.redirect_stdout(output):
            passes, coverage = dr._run_passes(None, "http://fixture/", tmp, 6,
                [("early", 0)], 1, None, None, None, time.monotonic(),
                viewport={"width": 100, "height": 100}, device_scale_factor=1)
        self.walk_output = output.getvalue()
        return demo.build_demo_summary(passes, {"fixture": {"ok": True}}), coverage

    def test_tab_with_no_first_sample_before_500_refuses_attempt(self):
        for offsets in ([500], [501], [500, 570]):
            with self.subTest(offsets=offsets):
                summary, coverage = self.walk(offsets, {"timeline": [200]})
                self.assertFalse(summary["ok"])
                self.assertIn("early/queries", summary["failed"])
                self.assertFalse(coverage[0]["ok"])
                self.assertTrue(coverage[1]["ok"])

    def test_tab_with_one_early_tick_passes_and_retains_every_late_tick(self):
        self.assertTrue(self.walk([499])[0]["ok"])
        for offsets in ([200, 500, 570], [500, 570, 499]):
            with self.subTest(offsets=offsets):
                summary, coverage = self.walk(offsets)
                self.assertTrue(summary["ok"])
                self.assertTrue(coverage[0]["ok"])
                self.assertEqual(len(coverage[0]["ticks"]), len(offsets))
                for i, offset in enumerate(offsets, start=1):
                    if offset >= 500:
                        self.assertIn(f"tick {i}: first sample={offset}ms -- EARLY WINDOW LIKELY MISSED",
                                      coverage[0]["detail"])
                        self.assertIn(f"tick {i}: first offset target=200ms achieved={offset}ms",
                                      self.walk_output)
                self.assertEqual(self.walk_output.count("-- EARLY WINDOW LIKELY MISSED (first sample"), 2)

    def test_missing_early_window_measurement_refuses(self):
        self.assertFalse(self.walk([])[0]["ok"])
        tab = demo.summarize_sweep_offset_coverage("queries", "early", [{}], 500)
        self.assertFalse(tab.get("ok", True))


class ArtifactSurvival(unittest.TestCase):
    def test_collect_names_and_refuses_previous_unarchived_run(self):
        source = (ROOT / "scripts/demo-rehearsal.sh").read_text()
        # Exercise the actual shell function, including its rsync/validation
        # path, without launching a VM or invoking the unrelated cleanup trap.
        function = source.split("collect_results() {", 1)[1].split("\n}\n", 1)[0]
        guard = ""
        if "refuse_previous_results() {" in source:
            guard = ("refuse_previous_results() {" + source.split(
                "refuse_previous_results() {", 1)[1].split("\n}\n", 1)[0] + "\n}\n")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old = root / "tests/results/demo_rehearsal"
            old.mkdir(parents=True)
            (old / "run.id").write_text("attempt-1\n")
            (old / "summary.json").write_text("previous evidence\n")
            cleanup = source.split("ephemeral_cleanup() {", 1)[1].split("\n}\n", 1)[0]
            state = root / "state.sh"
            state.write_text("reattachment evidence\n")
            command = '''
set -uo pipefail
synced_back=0; results_ok=0; target=fixture; remote_dir=fixture
PERSISTENT=1; demo_rehearsal_start_epoch=attempt-2; log=fixture.log
rsync() { mkdir -p tests/results/.demo_rehearsal_staging; }
python3() { echo 'complete fixture'; }
'''+guard+"collect_results() {"+function+'''\n}
collect_results
cleanup_done=0; STATE_FILE=state.sh; last_state=finished
'''+"ephemeral_cleanup() {"+cleanup+'''\n}
ephemeral_cleanup
[[ "$results_ok" == 0 ]]
'''
            proc = subprocess.run(["bash", "-c", command], cwd=tmp,
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual((old / "summary.json").read_text(), "previous evidence\n")
            self.assertEqual(state.read_text(), "reattachment evidence\n")
            self.assertIn("attempt-1", proc.stdout + proc.stderr)
            self.assertIn("tests/results/demo_rehearsal", proc.stdout + proc.stderr)

    def test_launch_refuses_before_contacting_the_box(self):
        for previous in ("demo_rehearsal", ".demo_rehearsal_staging"):
            with self.subTest(previous=previous):
                self.assert_launch_refuses_before_contacting_the_box(previous)

    def assert_launch_refuses_before_contacting_the_box(self, previous):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "scripts").mkdir()
            launcher = root / "scripts/demo-rehearsal.sh"
            launcher.write_text((ROOT / "scripts/demo-rehearsal.sh").read_text())
            old = root / "tests/results" / previous
            old.mkdir(parents=True)
            (old / "run.id").write_text("attempt-1\n")
            sweep = root / "tests/hetzner-sweep.sh"
            sweep.write_text("#!/bin/sh\ntouch contacted-box\nexit 1\n")
            sweep.chmod(0o755)
            (root / "bin").mkdir()
            for cmd in ("security", "ssh", "rsync", "git"):
                stub = root / "bin" / cmd
                stub.write_text("#!/bin/sh\nexit 1\n")
                stub.chmod(0o755)
            env = dict(os.environ, PGWT_BOX="fixture", PATH=f"{root / 'bin'}:{os.environ['PATH']}")
            proc = subprocess.run(["bash", str(launcher)], env=env,
                                  text=True, capture_output=True, timeout=10)
            self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
            self.assertIn("attempt-1", proc.stdout + proc.stderr)
            self.assertIn(previous, proc.stdout + proc.stderr)
            self.assertFalse((root / "contacted-box").exists())
            self.assertFalse((root / "tests/results/.demo-rehearsal-state.sh").exists())

    def test_refused_collection_names_kept_vm_and_preserves_state(self):
        source = (ROOT / "scripts/demo-rehearsal.sh").read_text()
        cleanup = source.split("ephemeral_cleanup() {", 1)[1].split("\n}\n", 1)[0]
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp) / "state.sh"
            state.write_text("reattachment evidence\n")
            command = '''
set -euo pipefail
cleanup_done=0; results_collection_refused=1; PERSISTENT=0
server_id=123456; server_name=fixture-vm; server_ip=192.0.2.1
STATE_FILE=state.sh; log=fixture.log
'''+"ephemeral_cleanup() {"+cleanup+'''\n}
ephemeral_cleanup
ephemeral_cleanup
'''
            proc = subprocess.run(["bash", "-c", command], cwd=tmp,
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(state.read_text(), "reattachment evidence\n")
            self.assertIn("fixture-vm (id=123456, ip=192.0.2.1)", proc.stderr)
            self.assertIn("tests/hetzner-vm.sh delete 123456", proc.stderr)
            self.assertEqual(len(proc.stderr.splitlines()), 2)

    def test_publication_preserves_all_evidence_and_refuses_a_racing_destination(self):
        import demo_rehearsal_orchestrator_lib as orchestrator
        with tempfile.TemporaryDirectory() as tmp:
            staging = Path(tmp) / "staging"
            staging.mkdir()
            (staging / "trace").mkdir()
            (staging / "trace/raw").write_bytes(b"raw evidence")
            (staging / ".metadata").write_text("hidden evidence")
            (staging / "run.id").write_text("attempt-1")
            destination = Path(tmp) / "results"
            orchestrator.publish_results_dir(staging, destination)
            self.assertFalse(staging.exists())
            self.assertEqual((destination / "trace/raw").read_bytes(), b"raw evidence")
            self.assertEqual((destination / ".metadata").read_text(), "hidden evidence")
            staging.mkdir()
            (staging / "run.id").write_text("attempt-2")
            with self.assertRaisesRegex(orchestrator.OrchestratorError, "attempt-1"):
                orchestrator.publish_results_dir(staging, destination)
            self.assertEqual((destination / "run.id").read_text(), "attempt-1")
            self.assertEqual((staging / "run.id").read_text(), "attempt-2")

    def test_rehearsal_trace_retention_defaults_on_at_every_launcher(self):
        assignments = [
            ("scripts/demo-rehearsal.sh", 'RETAIN_TRACE="${RETAIN_TRACE:-1}"'),
            ("scripts/demo-rehearsal-remote-run.sh", 'RETAIN_TRACE="${4:-1}"'),
            ("tests/demo_rehearsal.sh", 'RETAIN_TRACE="${RETAIN_TRACE:-1}"')]
        for filename, assignment in assignments:
            self.assertIn(assignment, (ROOT / filename).read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
