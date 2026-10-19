#!/usr/bin/env python3
"""Browser-free rehearsal wiring gates; --mutations proves each guard goes red.

Capture the actual CLI -> _run_passes -> run_tab calls for all 33 visits.
Reuse the live-smoke protocol model for tick/mount equivalence, without Chromium.
"""
import contextlib
import inspect
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import MagicMock, patch

import demo_rehearsal as dr
import test_ui_live_smoke_footage as protocol

ROOT = Path(__file__).resolve().parent
REQUIRED = ['--url', 'http://fixture/', '--trace-dir', '/fixture',
            '--daemon-log', '/fixture.log']
PASSES = [('early', 0), ('middle', 700), ('late', 1400)]
SIGNATURE = inspect.signature(dr.live_smoke.run_tab)


def load_source(module, source):
    candidate = types.ModuleType('rehearsal_candidate')
    candidate.__file__ = module.__file__
    exec(compile(source, module.__file__, 'exec'), candidate.__dict__)
    return candidate


def capture(test, module=dr, footage=False):
    parser = module.build_arg_parser()
    test.assertIn('--footage-only', parser._option_string_actions,
                  'rehearsal CLI must accept --footage-only')
    test.assertIs(parser.parse_args(REQUIRED).footage_only, False)
    calls, results = [], []

    def record(*args, **kwargs):
        calls.append((args, kwargs))
        bound = SIGNATURE.bind(*args, **kwargs)
        bound.apply_defaults()
        r = dr.lib.build_tab_result(
            tab_id=args[1], rendered_ok=True, rendered_detail='fixture',
            ticks_observed=args[4], console_errors=[], blink_ratio=0,
            color_violations=[], leak_before={}, leak_after={}, artifacts={},
            blink_pair_offsets_ms=[200] * args[4], ttfp_ms=100)
        r['blink_sweep']['ticks'] = [{
            'target_offsets_ms': list(dr.lib.SWEEP_OFFSETS_MS),
            'achieved_offsets_ms': list(dr.lib.SWEEP_OFFSETS_MS)}]
        if bound.arguments['footage_only']:
            dr.live_smoke._mark_footage_result(r, args[4], args[4], 'video.webm')
            r['blink_sweep']['ticks'] = []
        return r

    class WalkComplete(Exception):
        pass

    fake_api = types.ModuleType('playwright.sync_api')
    fake_api.sync_playwright = MagicMock()
    browser = fake_api.sync_playwright.return_value.__enter__.return_value.chromium.launch.return_value
    with tempfile.TemporaryDirectory(dir=ROOT) as out, contextlib.ExitStack() as stack:
        argv = ['demo_rehearsal.py', *REQUIRED, '--out-dir', out]
        if footage:
            argv.append('--footage-only')
        stack.enter_context(patch.object(sys, 'argv', argv))
        stack.enter_context(patch.dict(sys.modules, {'playwright.sync_api': fake_api}))
        stack.enter_context(patch.object(module.live_smoke, '_require_playwright'))
        stack.enter_context(patch.object(module.live_smoke, '_assert_workload_alive'))
        stack.enter_context(patch.object(module, '_assert_daemon_alive'))
        stack.enter_context(patch.object(module.live_smoke, 'run_tab', side_effect=record))
        stack.enter_context(patch.object(module.threading, 'Thread'))
        stack.enter_context(patch.object(module.time, 'monotonic', return_value=0))
        sleep = stack.enter_context(patch.object(module.time, 'sleep'))
        stack.enter_context(patch.object(module.drlib, 'plan_passes', return_value=(6, PASSES)))
        # Keep the real walker while intercepting its caller in main().
        real_walk = module._run_passes
        def from_main(*args, **kwargs):
            results.extend(real_walk(*args, **kwargs))
            raise WalkComplete
        stack.enter_context(patch.object(module, '_run_passes', side_effect=from_main))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        with test.assertRaises(WalkComplete):
            module.main()
        test.assertEqual(sleep.call_args_list, [unittest.mock.call(700), unittest.mock.call(1400)])
        test.assertEqual(len(calls), 3 * len(dr.lib.TABS))
        for (name, _), start in zip(PASSES, range(0, len(calls), len(dr.lib.TABS))):
            for tab, (args, kwargs) in zip(dr.lib.TABS, calls[start:]):
                test.assertEqual(args, (browser, tab, 'http://fixture/',
                    os.path.join(out, name), 6, dr.lib.FIRST_DATA_TIMEOUT_S,
                    dr.lib.BLINK_THRESHOLD), f'{name}/{tab}: positional contract changed')
                expected = dict(viewport={'width': 1710, 'height': 981},
                                device_scale_factor=2, diag_data=False)
                if footage:
                    expected['footage_only'] = True
                test.assertEqual(kwargs, expected, f'{name}/{tab}: run_tab arguments changed')
                bound = SIGNATURE.bind(*args, **kwargs)
                bound.apply_defaults()
                test.assertIs(bound.arguments['footage_only'], footage, f'{name}/{tab}')
    return results


def assert_reporting(test, module=dr, summary_module=None):
    passes, coverage = capture(test, module, True)
    summary = (summary_module or module.drlib).build_demo_summary(
        passes, {'fixture': {'ok': True}}, expected_tabs_per_pass=len(dr.lib.TABS))
    test.assertIs(summary['ok'], False, 'footage aggregate must never pass')
    test.assertNotIn('REHEARSAL: PASS', module.drlib.verdict_line(summary))
    test.assertEqual(len(coverage), 33)
    for p in passes:
        for tab, result in p['tabs'].items():
            test.assertIs(result['ok'], False)
            for key in ('no_blink', 'capture_budget', 'frame_dims', 'frame_spacing',
                        'sweep_offset_coverage'):
                test.assertIsNone(result[key]['ok'], f'{p["pass"]}/{tab}/{key}')
                test.assertEqual(result[key]['detail'], dr.live_smoke.FOOTAGE_NOT_EVALUATED)
    for check in coverage:
        test.assertIsNone(check['ok'])
        test.assertEqual(check['detail'], dr.live_smoke.FOOTAGE_NOT_EVALUATED)


def assert_shell(test, source=None):
    source = source or (ROOT / 'demo_rehearsal.sh').read_text()
    # Execute the actual option parser and Python command, omitting daemon setup.
    parser = source.split('PM_PID=""', 1)[1].split('\nif [[ -z "$PM_PID" ]]', 1)[0]
    command = source.split('python3 -u "$SCRIPT_DIR/demo_rehearsal.py"', 1)[1].split('\nDEMO_RC=', 1)[0]
    script = '''set -uo pipefail
python3() { printf '%s\\n' "$@"; }
SCRIPT_DIR=fixture BASE_URL=fixture TRACE_DIR=fixture DAEMON_LOG=fixture
DURATION_MIN=35 PGBENCH_PID=1 WORKLOAD_PID=2 TRACER_PID=3 PM_PID=4
DIAG_CLI_ARGS=() TAG_CLI_ARGS=()
''' + parser + '\npython3 -u "$SCRIPT_DIR/demo_rehearsal.py"' + command
    for flags in ([], ['--footage-only']):
        proc = subprocess.run(['bash', '-c', script, 'fixture', *flags],
                              text=True, capture_output=True, cwd=ROOT)
        test.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        test.assertEqual(proc.stdout.splitlines().count('--footage-only'), len(flags))


class RehearsalFootageTests(unittest.TestCase):
    def test_default_call_contract(self):
        self.assertIs(inspect.signature(dr._run_passes).parameters['footage_only'].default,
                      False)
        passes, _ = capture(self)
        self.assertTrue(dr.drlib.build_demo_summary(passes, {'fixture': {'ok': True}})['ok'])

    def test_all_passes_all_tabs(self):
        capture(self, footage=True)

    def test_aggregate_and_screenshot_reporting(self):
        assert_reporting(self)

    def test_cli_rejects_incompatible_options_before_setup(self):
        for extra in (['--diag-data'], ['--inject-blink-ms', '400'],
                      ['--inject-blink-ms=400']):
            with self.subTest(extra=extra), \
                    patch.object(sys, 'argv', ['demo_rehearsal.py', *REQUIRED,
                                              '--footage-only', *extra]), \
                    patch.object(dr.lib, 'reset_output_dir') as reset, \
                    contextlib.redirect_stderr(io.StringIO()) as err:
                with self.assertRaises(SystemExit) as raised:
                    dr.main()
                self.assertEqual(raised.exception.code, 2)
                self.assertIn('--footage-only cannot be combined with', err.getvalue())
                reset.assert_not_called()

    def test_shell_forwarding(self):
        assert_shell(self)

    def test_tick_targets_and_mount_waits_equivalent(self):
        capture(self, footage=True)
        protocol.assert_cadence(self, dr.live_smoke, protocol.baseline())


def mutations():
    test = unittest.TestCase()
    def mutated(module, old, new):
        text = Path(module.__file__).read_text()
        test.assertEqual(text.count(old), 1, 'mutation site must be unique')
        return load_source(module, text.replace(old, new))
    def reject(name, bad, restored):
        try:
            bad()
        except AssertionError as error:
            print(f'RED [{name}]: {str(error)[:500]}')
        else:
            raise AssertionError(f'NEGATIVE CONTROL SURVIVED: {name}')
        restored()
        print(f'GREEN [{name}]: restored production source')

    site = '**({"footage_only": True} if footage_only else {})'
    reject('default: enable footage when unset',
           lambda: capture(test, mutated(dr, site, '**{"footage_only": True}')),
           lambda: capture(test))
    reject('A: forward footage only to early pass',
           lambda: capture(test, mutated(dr, site,
               '**({"footage_only": True} if footage_only and name == "early" else {})'), True),
           lambda: capture(test, footage=True))
    reject('B: force aggregate pass despite footage refusals',
           lambda: assert_reporting(test, summary_module=mutated(dr.drlib,
               '"ok": ok,\n        "passes":', '"ok": True,\n        "passes":')),
           lambda: assert_reporting(test))
    reject('reporting: erase screenshot coverage explanation',
           lambda: assert_reporting(test, mutated(dr,
               '"detail": live_smoke.FOOTAGE_NOT_EVALUATED', '"detail": None')),
           lambda: assert_reporting(test))
    for name, old, new in [
        ('C: double footage tick targets',
         '_wait_for_tick(page, tick_baseline + i, TICK_TIMEOUT_S)',
         '_wait_for_tick(page, tick_baseline + (2 * i if footage_only else i), TICK_TIMEOUT_S)'),
        ('C: change footage mount timeout',
         '_wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, TICK_TIMEOUT_S,',
         '_wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, TICK_TIMEOUT_S + (1 if footage_only else 0),')]:
        reject(name, lambda: protocol.assert_cadence(test, mutated(dr.live_smoke, old, new),
                                                    protocol.baseline()),
               lambda: protocol.assert_cadence(test, dr.live_smoke, protocol.baseline()))
    shell = (ROOT / 'demo_rehearsal.sh').read_text()
    reject('shell: drop footage argument at Python invocation',
           lambda: assert_shell(test, shell.replace(
               ' "${FOOTAGE_CLI_ARGS[@]+${FOOTAGE_CLI_ARGS[@]}}"', '')),
           lambda: assert_shell(test))
    print('7/7 mutations rejected; 7/7 restored guards passed')


if __name__ == '__main__':
    if sys.argv[1:] == ['--mutations']:
        mutations()
    else:
        unittest.main(verbosity=2)
