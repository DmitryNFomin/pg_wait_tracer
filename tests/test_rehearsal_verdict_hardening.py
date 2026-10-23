#!/usr/bin/env python3
"""Verdict refusals exercised through the production aggregators and launcher.

No browser, SSH or capture: the pass walker receives recorded tab measurements,
and collect_results receives a complete local fixture through a fake rsync.
"""
import contextlib
import ast
import copy
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import MagicMock, patch

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


def ui_measurements():
    disclosure = {
        'text': 'Showing 100 of 265,756', 'rendered_rows': 100, 'response_rows': 100,
        'response_total': 265756, 'truncated': True, 'fixture_installed': True,
        'visible': True, 'fully_visible': True, 'outside_tbody': True,
        'viewport': [1710, 981, 2],
        'scroll_affordance': {'has_more': True, 'scroll_height': 4000,
                             'client_height': 100, 'shadow': 'inset 0 -1px black'},
    }
    fold = {'ok': True, 'viewport': [1710, 981, 2], 'fixture_rows': 100,
            'disclosure': copy.deepcopy(disclosure), 'scroll_height': 981, 'scroll_y': 0,
            'readout_bottom': 980, 'chart': {
                'rendered_bars': 3, 'minimum_bar_height': 10, 'stable_after_resize': True,
                'readout_clickable': True, 'scroll_height_after_click': 981,
                'scroll_y_after_click': 0, 'readout_bottom_after_click': 981}}
    lists = {'ok': True}
    disclosures = {'ok': True}
    for view, viewport, minimum in [('snapshot', [1280, 800, 1], 1), ('demo', [1710, 981, 2], 2)]:
        lists[view] = {'viewport': viewport, 'header': {'full': True}, 'full_rows': minimum,
                       'rows': [{'full': True}] * minimum, 'scroll_y': 0, 'list_scroll_top': 0,
                       'required_list_height': 80, 'list_height': 100, 'stable_after_resize': True}
        disclosures[view] = {**copy.deepcopy(disclosure), **copy.deepcopy(lists[view]),
                             'full_rows': 1, 'rows': [{'full': True}]}
    return {'waterfall_fold': fold, 'execution_list_full_rows': lists,
            'execution_truncation_disclosure': disclosures}


class RehearsalUIGates(unittest.TestCase):
    names = ('waterfall_fold', 'execution_list_full_rows', 'execution_truncation_disclosure')

    def controls(self, name):
        return {control: {'ok': False, 'detail': 'observed assertion failure: ' + control}
                for control in demo.UI_GATE_CONTROLS[name]}

    def evaluate(self, name, measured, controls):
        return demo.evaluate_ui_gate(name, measured, controls, demo.UI_GATE_CONTROLS[name])

    def test_each_ui_check_is_mandatory_by_name(self):
        for name in self.names:
            with self.subTest(name=name):
                checks = {key: {'ok': True} for key in demo.REQUIRED_EXTRA_CHECKS if key != name}
                summary = demo.build_demo_summary(
                    [{'pass': 'early', 'tabs': {'queries': clean_tab()}}], checks,
                    required_checks=demo.REQUIRED_EXTRA_CHECKS)
                self.assertFalse(summary['ok'])
                self.assertIn('missing_required_check:' + name, summary['failed'])

    def test_main_registers_live_ui_checks_and_requires_them_in_summary(self):
        tree = ast.parse((ROOT / 'tests/demo_rehearsal.py').read_text())
        main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main')
        calls = [node for node in ast.walk(main) if isinstance(node, ast.Call)]
        ui_calls = [node for node in calls if ast.unparse(node.func) == '_run_ui_gates']
        self.assertEqual(len(ui_calls), 1)
        self.assertEqual(ast.unparse(ui_calls[0].args[0]), 'args.url')
        assignments = [node for node in ast.walk(main) if isinstance(node, ast.Assign)
                       and node.value is ui_calls[0]]
        self.assertEqual([ast.unparse(node.targets[0]) for node in assignments], ['ui_checks'])
        for name in self.names:
            inserted = [node for node in ast.walk(main) if isinstance(node, ast.Assign)
                        and ast.unparse(node.targets[0]) == f"extra_checks['{name}']"]
            self.assertEqual(len(inserted), 1)
            self.assertEqual(ast.unparse(inserted[0].value), f"ui_checks['{name}']")
        summary = [node for node in calls if ast.unparse(node.func) == 'drlib.build_demo_summary']
        self.assertEqual(len(summary), 1)
        self.assertEqual(ast.unparse(summary[0].args[1]), 'extra_checks')
        required = next(kw.value for kw in summary[0].keywords if kw.arg == 'required_checks')
        self.assertEqual(ast.unparse(required), 'drlib.REQUIRED_EXTRA_CHECKS')

    def test_ui_evaluator_refuses_every_bypass(self):
        for name in self.names:
            measured = ui_measurements()[name]
            good = self.controls(name)
            self.assertTrue(self.evaluate(name, measured, good)['ok'])
            control = next(iter(good))
            missing = dict(good)
            del missing[control]
            missing_measurement = copy.deepcopy(measured)
            del missing_measurement['chart' if name == 'waterfall_fold' else 'snapshot']
            cases = [('none', None, good), ('empty', {}, good),
                     ('missing_ok', {k: v for k, v in measured.items() if k != 'ok'}, good),
                     ('missing_measurement', missing_measurement, good),
                     ('missing_control', measured, missing),
                     ('unexpected_control', measured, dict(good, unexpected={'ok': False, 'detail': 'failed'}))]
            for label, record in [('passed', {'ok': True, 'detail': 'survived'}),
                                  ('empty_text', {'ok': False, 'detail': ''}),
                                  ('whitespace_text', {'ok': False, 'detail': '  '}),
                                  ('non_string_text', {'ok': False, 'detail': True}),
                                  ('missing_text', {'ok': False}),
                                  ('unrecorded_outcome', {'detail': 'failed'})]:
                cases.append((label, measured, dict(good, **{control: record})))
            for label, readings, controls in cases:
                with self.subTest(name=name, bypass=label):
                    result = self.evaluate(name, readings, controls)
                    self.assertFalse(result['ok'])
                    self.assertTrue(result['detail'])

    def test_ui_evaluator_refuses_caller_required_controls_bypass(self):
        measured = ui_measurements()['execution_truncation_disclosure']
        cases = [
            ('shrunken_controls', 'execution_truncation_disclosure', ('disclosure_title_only',)),
            ('empty_controls', 'execution_truncation_disclosure', ()),
            ('renamed_control', 'execution_truncation_disclosure',
             ('chart_floor_240', 'made_up_control')),
            ('unknown_gate', 'not_a_gate', ()),
        ]
        for bypass, name, required in cases:
            with self.subTest(bypass=bypass):
                # Both control-name checks agree with the caller's supplied bar.
                controls = {control: {'ok': False, 'detail': 'observed assertion failure: ' + control}
                            for control in required}
                result = demo.evaluate_ui_gate(name, measured, controls, required)
                self.assertFalse(result['ok'])
                self.assertEqual(result['detail'], 'unknown gate or incorrect required controls')

    def test_ui_evaluator_refuses_broken_product_measurements(self):
        cases = [
            ('waterfall_fold', ('scroll_height',), 982),
            ('waterfall_fold', ('scroll_y',), 1),
            ('waterfall_fold', ('chart', 'scroll_height_after_click'), 982),
            ('waterfall_fold', ('chart', 'scroll_y_after_click'), 1),
            ('waterfall_fold', ('chart', 'readout_bottom_after_click'), 982),
            ('waterfall_fold', ('chart', 'rendered_bars'), 0),
            ('waterfall_fold', ('disclosure', 'fixture_installed'), False),
            ('execution_list_full_rows', ('snapshot', 'full_rows'), 0),
            ('execution_list_full_rows', ('demo', 'full_rows'), 1),
            ('execution_list_full_rows', ('demo', 'header', 'full'), False),
            ('execution_list_full_rows', ('demo', 'stable_after_resize'), False),
            ('execution_truncation_disclosure', ('snapshot', 'visible'), False),
            ('execution_truncation_disclosure', ('demo', 'fully_visible'), False),
            ('execution_truncation_disclosure', ('demo', 'outside_tbody'), False),
            ('execution_truncation_disclosure', ('demo', 'text'), 'Showing 265,756 of 265,756'),
            ('execution_truncation_disclosure', ('demo', 'fixture_installed'), False),
        ]
        for view in ('snapshot', 'demo'):
            for path, value in [(('header', 'full'), False), (('full_rows',), 0),
                                (('rows',), []), (('list_height',), 79),
                                (('required_list_height',), 0), (('scroll_y',), 1),
                                (('list_scroll_top',), 1), (('stable_after_resize',), False)]:
                cases.append(('execution_truncation_disclosure', (view, *path), value))
        for name, path, value in cases:
            with self.subTest(name=name, path=path):
                measured = ui_measurements()[name]
                target = measured
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = value
                self.assertFalse(self.evaluate(name, measured, self.controls(name))['ok'])

    def test_ui_evaluator_requires_recorded_capped_list_measurements(self):
        name = 'execution_truncation_disclosure'
        for view in ('snapshot', 'demo'):
            for field in ('header', 'full_rows', 'rows', 'list_height', 'required_list_height',
                          'scroll_y', 'list_scroll_top', 'stable_after_resize'):
                with self.subTest(view=view, missing=field):
                    measured = ui_measurements()[name]
                    del measured[view][field]
                    self.assertFalse(self.evaluate(name, measured, self.controls(name))['ok'])

    def test_dense_fixture_refuses_empty_uncapped_or_untruncated_responses(self):
        self.assertTrue(demo.capped_fixture_ok(100, 100, 265756, True))
        for values in [(0, 0, 265756, True), (100, 0, 265756, True),
                       (100, 100, 100, True), (100, 100, 99, True),
                       (100, 100, None, True), (100, 100, 265756, False)]:
            with self.subTest(values=values):
                self.assertFalse(demo.capped_fixture_ok(*values))

    def run_browser_stub(self, *, exception=None, launch_error=None, mutate=None):
        from web_unit import execution_list_browser as gates
        manager = MagicMock()
        launcher = manager.__enter__.return_value.chromium.launch
        if launch_error:
            launcher.side_effect = launch_error
        measured = ui_measurements()
        for name in self.names:
            failures = {key: value['detail'] for key, value in self.controls(name).items()}
            if name == 'execution_truncation_disclosure':
                for view in ('snapshot', 'demo'):
                    measured[name][view]['negative_control_failures'] = {
                        control: text for control, text in failures.items()
                        if view == 'snapshot' or control != 'list_floor_60'}
            else:
                measured[name]['negative_control_failures'] = failures
        if mutate:
            mutate(measured)
        module = MagicMock(sync_playwright=lambda: manager)
        with patch.dict(sys.modules, {'playwright': MagicMock(), 'playwright.sync_api': module}), \
                patch.object(gates, 'run_waterfall_fold_gate',
                             side_effect=exception, return_value=measured['waterfall_fold']), \
                patch.object(gates, 'run_execution_list_gates',
                             return_value=measured['execution_list_full_rows']), \
                patch.object(gates, 'run_execution_disclosure_gates',
                             return_value=measured['execution_truncation_disclosure']) as disclosures, \
                contextlib.redirect_stdout(io.StringIO()):
            result = dr._run_ui_gates('http://live-bridge:8384/')
        if not exception and not launch_error:
            disclosures.assert_called_once_with(launcher.return_value, 'http://live-bridge:8384/',
                                                 include_fitting=False)
        return result

    def test_ui_registration_retains_measurements_controls_and_live_url(self):
        checks = self.run_browser_stub()
        self.assertEqual(set(checks), set(self.names))
        for name, check in checks.items():
            self.assertTrue(check['ok'], check['detail'])
            self.assertEqual(set(check['negative_control_failures']), set(demo.UI_GATE_CONTROLS[name]))
            self.assertEqual(check['url'], 'http://live-bridge:8384/#tab=waterfall&live=1&span=900')

    def test_browser_exceptions_fail_all_three_checks(self):
        for label, kwargs in [('timeout', {'exception': TimeoutError('bridge down')}),
                              ('empty_capture', {'exception': AssertionError('no executions')}),
                              ('missing_chromium', {'launch_error': RuntimeError('chromium absent')})]:
            with self.subTest(label=label):
                checks = self.run_browser_stub(**kwargs)
                self.assertEqual(set(checks), set(self.names))
                for check in checks.values():
                    self.assertFalse(check['ok'])
                    self.assertIn('http://live-bridge:8384/', check['url'])
                    self.assertTrue(check['detail'])

    def test_live_registration_refuses_controls_that_did_not_fire(self):
        for name, view in [('waterfall_fold', None), ('execution_list_full_rows', None),
                           ('execution_truncation_disclosure', 'snapshot'),
                           ('execution_truncation_disclosure', 'demo')]:
            for control in demo.UI_GATE_CONTROLS[name]:
                if view == 'demo' and control == 'list_floor_60':
                    continue
                for bad_text in (None, '', True):
                    with self.subTest(name=name, view=view, control=control, bad_text=bad_text):
                        def mutate(measured):
                            record = measured[name][view] if view else measured[name]
                            failures = record['negative_control_failures']
                            if bad_text is None:
                                del failures[control]
                            else:
                                failures[control] = bad_text
                        self.assertFalse(self.run_browser_stub(mutate=mutate)[name]['ok'])

    def test_missing_playwright_registers_three_failures(self):
        with patch.dict(sys.modules, {'playwright.sync_api': None}), \
                contextlib.redirect_stdout(io.StringIO()):
            checks = dr._run_ui_gates('http://live-bridge:8384/')
        self.assertEqual(set(checks), set(self.names))
        self.assertTrue(all(not check['ok'] and 'ModuleNotFoundError' in check['detail']
                            for check in checks.values()))


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
