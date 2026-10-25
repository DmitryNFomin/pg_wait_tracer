#!/usr/bin/env python3
"""Exercise the real run_tab against a deterministic browser protocol model.

The frozen 907e070 baseline fixture is intentional: comparing two invocations of the new
code would not detect a default-path regression shared by both. No browser
helpers, capture functions, navigation or verdict functions are stubbed.
Run directly; --mutations additionally proves the five guards reject defects.
"""
import contextlib
import io
import json
from pathlib import Path
import hashlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image
import ui_live_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
NOTE = 'not evaluated in footage mode'


def load_source(source):
    module = types.ModuleType('footage_candidate')
    module.__file__ = str(ROOT / 'tests/ui_live_smoke.py')
    exec(compile(source, module.__file__, 'exec'), module.__dict__)
    return module


def baseline():
    module = load_source(Path(smoke.__file__).read_text())
    path = ROOT / 'tests/fixtures/ui_live_smoke_907e070_run_tab.py'
    exec(compile(path.read_text(), str(path), 'exec'), module.__dict__)
    return module


class Page:
    """5s ticks, mounts after 150ms, 90ms screenshot cost. Logs all calls."""
    video = None

    def __init__(self, tab, viewport):
        self.tab, self.viewport_size = tab, viewport
        self.now = 1000
        self.calls = []
        self.mount_waits = []
        self.live = tab not in {'timeline', 'transitions', 'concurrency',
                                'waterfall', 'scatter', 'matrix'}
        self.origin = None
        self.target = 0
        self.mount = {'id': tab, 'seq': 1, 'at': self.now}
        pixels = np.indices((20, 30)).sum(axis=0).astype('uint8') * 5
        buf = io.BytesIO()
        Image.fromarray(pixels).save(buf, format='PNG')
        self.png = buf.getvalue()

    def log(self, name, *args):
        self.calls.append((name, args, self.now))

    def on(self, *args):
        pass

    def goto(self, url):
        self.log('goto', url)

    def wait_for_selector(self, selector, **kw):
        self.log('ready', selector, kw)

    def query_selector(self, selector):
        self.log('query', selector)
        self.selected = selector
        return self

    def click(self, selector='row'):
        if selector == 'row':
            selector = self.selected
        self.log('click', selector)
        self.mount = {'id': self.tab, 'seq': 1, 'at': self.now}
        if selector == '#live-btn':
            self.live = True

    def scroll_into_view_if_needed(self):
        self.log('scroll')

    def bounding_box(self):
        self.log('box')
        return {'x': 0, 'y': 0, 'width': 30, 'height': 20}

    # Two JS shapes reach this fake, and they are told apart by STRUCTURE,
    # never by "whatever the last whitespace-delimited token happens to be".
    # The old code did the latter -- `int(js.rsplit(' ', 1)[1])` -- which
    # worked only while every wait_for_function in the walker ended in a tick
    # count. U2's "Load flow variants" settle wait ends in `variants')`, so
    # the fake died with `ValueError: invalid literal for int() with base 10:
    # "variants')"`: a crash in the MODEL of the browser, blamed on the walker.
    #
    # An unrecognised shape now REFUSES BY NAME instead of guessing. That is
    # deliberate: this file freezes a protocol, so a call it cannot model is
    # something a human has to teach it, and a fake that silently treats an
    # unknown wait as a zero-tick no-op would let a real new browser
    # interaction slip past the very comparison this file exists to make.
    TICK_WAIT_PREFIX = ('window.__uiLiveTicks && '
                        'window.__uiLiveTicks.length >= ')

    def wait_for_function(self, js, **kw):
        if js.startswith(self.TICK_WAIT_PREFIX):
            self.target = int(js[len(self.TICK_WAIT_PREFIX):].strip())
            if self.origin is None:
                self.origin = self.now
            self.now = max(self.now, self.origin + self.target * 5000)
            self.log('tick', self.target, kw)
            return
        if 'dfg-load-variants' in js:
            # The walker clicks the button, then waits for it to go away and
            # the panel to stop saying "Loading flow variants" (#327's hang
            # regression guard). The fake resolves it immediately and does
            # NOT advance its clock, so every later call keeps the timestamp
            # it had before this step existed -- the delta against the frozen
            # oracle stays a pure INSERTION rather than a reflow of the whole
            # tail, which is what makes it reviewable.
            self.log('await', 'dfg-load-variants settled', kw)
            return
        raise AssertionError(
            'test_ui_live_smoke_footage: this fake Page cannot model '
            'wait_for_function(%r). Teach it the new shape -- do not let it '
            'guess, and do not delete this refusal: an unmodelled browser '
            'call is exactly what the frozen-protocol comparison below is '
            'for.' % (js,))

    def wait_for_timeout(self, ms):
        self.log('sleep', ms)
        self.now += ms

    def evaluate(self, js, arg=None):
        self.log('evaluate', js, arg)
        if js == 'Date.now()':
            return self.now
        if 'window.innerWidth' in js:
            return {'w': self.viewport_size['width'],
                    'h': self.viewport_size['height'], 'dpr': 1}
        if js == smoke.TICK_HOOK_JS:
            return None
        if js == smoke._LIVE_ACTIVE_JS:
            return self.live
        if js == smoke.VIEW_MOUNT_JS:
            if self.target and self.now >= self.origin + self.target * 5000 + 150:
                self.mount = {'id': self.tab, 'seq': self.target + 1,
                              'at': self.origin + self.target * 5000 + 150}
            return {'missing': False, 'mount': self.mount}
        if js == smoke.LEAK_PROBE_JS:
            return {'charts': 1, 'uplots': 1, 'pending': 0}
        if js == 'window.__uiLiveTicks.length':
            return 0
        if js == 'idx => window.__uiLiveTicks[idx]':
            return self.origin + (arg + 1) * 5000
        if js == smoke._ATOMIC_PANEL_SNAPSHOT_JS:
            return {'present': True, 'canvas': False, 'hasContent': True}
        if js in smoke.PANEL_CHECKS.values():
            return {'ok': True, 'detail': 'data'}
        if js == smoke.LEGEND_COLORS_JS:
            return {'CPU': 'red'}
        raise AssertionError(f'unmodelled evaluate: {js}')

    def screenshot(self, **kw):
        self.log('screenshot', kw)
        self.now += 90
        return self.png


class Browser:
    def __init__(self, tab):
        self.tab = tab

    def new_context(self, **kw):
        self.options = kw
        self.page = Page(self.tab, kw['viewport'])
        video = Path(kw['record_video_dir']) / 'raw.webm'
        video.write_bytes(b'protocol-model video')
        self.page.video = types.SimpleNamespace(path=lambda: str(video))
        return self

    def add_init_script(self, script):
        self.page.log('init', script)

    def new_page(self):
        return self.page

    def close(self):
        self.page.log('close')


def drive(module, tab, footage=False):
    browser = Browser(tab)
    kwargs = {'footage_only': True} if footage else {}
    wait_mount = module._wait_for_mount_at_or_after

    def observe_mount(page, *args, **kw):
        mount = wait_mount(page, *args, **kw)
        page.mount_waits.append((args, kw, mount))
        return mount

    with tempfile.TemporaryDirectory(dir=ROOT / 'tests') as out:
        with patch.object(module, '_wait_for_mount_at_or_after', observe_mount), \
                patch.object(module.time, 'monotonic', lambda: browser.page.now / 1000), \
                contextlib.redirect_stdout(io.StringIO()):
            result = module.run_tab(browser, tab, 'http://fixture/', out, 6, 60,
                                    smoke.lib.BLINK_THRESHOLD, **kwargs)
        result['artifacts'] = {'frames': [Path(p).name for p in result['artifacts']['frames']],
                               'video': None}
    return browser.page, result


def calls(page, kind):
    return [c for c in page.calls if c[0] == kind]


# The ONE deliberate protocol change since the 907e070 oracle was frozen.
#
# U2 (agent/decided-smalls) lifted the standing ban on clicking "Load flow
# variants" during the walk -- #327's hang is fixed, #269 replaced the bubble
# sort, and tests/ui_live_smoke.py now exercises the button so the walk's own
# gate covers it instead of presenter choreography. On the transitions tab the
# walker therefore performs three new browser calls.
#
# WHY THIS IS PINNED HERE RATHER THAN RE-FREEZING THE ORACLE. The fixture says
# in its own header: "Never update this oracle to make a candidate pass: it
# represents the pre-footage path." Re-freezing it against today's run_tab
# would satisfy this test by destroying what it measures -- the comparison
# would become new-code-vs-new-code, which the module docstring already warns
# "would not detect a default-path regression shared by both". So the oracle
# stays at 907e070 and the intended delta is written down instead.
#
# This is STRICTER than the equality it replaces, in both directions:
#   * every other tab must still be byte-identical, and
#   * the three calls must be PRESENT and in this order -- quietly dropping
#     the variants click fails too, which a tolerant "ignore transitions"
#     exception would not catch.
# Anything else differing anywhere still fails as before.
EXPECTED_WALK_ADDITIONS = {
    'transitions': [
        ('query', ('#dfg-load-variants',)),
        ('click', ('#dfg-load-variants',)),
        ('await', ('dfg-load-variants settled', {'timeout': 30000})),
    ],
}

# The same change also adds one key to the transitions VERDICT: how long the
# click took, which is the number #327's regression guard reads. It is a
# time.monotonic() delta, so it is pinned by PRESENCE AND TYPE and never by
# value -- asserting a literal here would be pinning the clock, which is how a
# test becomes timing-flaky on a loaded runner.
EXPECTED_RESULT_ADDITIONS = {'transitions': ('variants_click_ms',)}


def strip_expected_result_keys(test, tab, result):
    """result minus this tab's deliberate new keys, after checking each one."""
    out = dict(result)
    for key in EXPECTED_RESULT_ADDITIONS.get(tab, ()):
        test.assertIn(key, out,
                      f'{tab}: the deliberate verdict key {key!r} is missing. '
                      f'If the walk dropped it, delete it from '
                      f'EXPECTED_RESULT_ADDITIONS in the same commit.')
        value = out.pop(key)
        test.assertIsInstance(value, float,
                              f'{tab}: {key} must be a float duration')
        test.assertGreaterEqual(value, 0.0,
                                f'{tab}: {key} must not be negative')
    return out


def strip_expected_additions(tab, after_calls):
    """after_calls minus this tab's deliberate additions, or a reason why not.

    Returns (stripped, None) on success and (None, reason) when the additions
    are absent, out of order, or not contiguous -- never a silent pass-through,
    because "the addition was not found" must fail rather than compare as if
    nothing had changed.
    """
    want = EXPECTED_WALK_ADDITIONS.get(tab)
    if not want:
        return list(after_calls), None
    n = len(want)
    for i in range(len(after_calls) - n + 1):
        window = after_calls[i:i + n]
        if [(c[0], c[1]) for c in window] != want:
            continue
        # The additions must not have reflowed the clock: they are logged at
        # the timestamp already current, so every later call keeps its own.
        return list(after_calls[:i]) + list(after_calls[i + n:]), None
    return None, (
        f'{tab}: the deliberate "Load flow variants" calls are missing from '
        f'the walk (expected the contiguous run {want!r}). If the walk '
        f'genuinely dropped that step, delete this tab\'s entry from '
        f'EXPECTED_WALK_ADDITIONS in the same commit -- do not relax this '
        f'check.')


def assert_normal(test, module, base):
    for tab in smoke.lib.TABS:
        before, expected = drive(base, tab)
        after, result = drive(module, tab)
        test.assertEqual(len(calls(after, 'screenshot')), 36 if tab == 'timeline' else 30,
                         f'{tab}: default screenshot count changed')
        stripped, why = strip_expected_additions(tab, after.calls)
        test.assertIsNotNone(stripped, why)
        test.assertEqual(stripped, before.calls,
                         f'{tab}: default timing/protocol changed beyond the '
                         f'deliberate additions in EXPECTED_WALK_ADDITIONS')
        test.assertEqual(strip_expected_result_keys(test, tab, result), expected,
                         f'{tab}: default result changed beyond the '
                         f'deliberate keys in EXPECTED_RESULT_ADDITIONS')


def assert_zero(test, module):
    for tab in smoke.lib.TABS:
        page, result = drive(module, tab, True)
        test.assertEqual(len(calls(page, 'screenshot')), 0, f'{tab}: footage screenshot count')
        test.assertEqual(result['artifacts']['frames'], [])


def assert_reporting(test, module):
    for tab in smoke.lib.TABS:
        page, result = drive(module, tab, True)
        test.assertIs(result['ok'], False)
        test.assertFalse(result['known_failing'])
        test.assertFalse(result['xpass'])
        test.assertEqual(result['ticks_observed'], 6)
        for key in ('no_blink', 'capture_budget', 'frame_dims', 'frame_spacing'):
            test.assertIsNone(result[key]['ok'], f'{tab}: {key} must be unevaluated')
            test.assertEqual(result[key]['detail'], NOTE, f'{tab}: {key} needs explicit report')
        test.assertIsNone(result['no_blink']['measured']['ok'])
        test.assertEqual(result['no_blink']['measured']['detail'], NOTE)
        test.assertEqual(result['blink_sweep']['detail'], NOTE)
        test.assertEqual(result['blink_sweep']['ticks'], [])
        if tab == 'timeline':
            test.assertEqual(len(result['pre_mount_diagnostic']), 6)
            test.assertTrue(all(d['note'] == NOTE for d in result['pre_mount_diagnostic']))
        test.assertTrue(result['footage_complete'])
        for key in ('rendered', 'clean', 'no_leak', 'color_stability', 'first_paint'):
            test.assertIs(result[key]['ok'], True, key)


def assert_cadence(test, module, base):
    for tab in smoke.lib.TABS:
        before, _ = drive(base, tab)
        after, _ = drive(module, tab, True)
        for kind in ('goto', 'click', 'ready', 'tick'):
            test.assertEqual(calls(after, kind), calls(before, kind), f'{tab}: {kind} cadence/navigation')
        # Compare requested mount waits and returned mounts. Removing the
        # Timeline pre-mount capture can naturally require one more 50ms poll.
        test.assertEqual(after.mount_waits, before.mount_waits, f'{tab}: mount waits changed')
        legends = lambda p: [c[2] for c in calls(p, 'evaluate') if c[1][0] == smoke.LEGEND_COLORS_JS]
        test.assertEqual([t - 90 for t in legends(before)], legends(after),
                         f'{tab}: sweep dwell/cadence changed (only final capture cost may disappear)')


class FootageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = baseline()

    def test_default_matches_frozen_baseline(self):
        assert_normal(self, smoke, self.base)

    def test_footage_zero_screenshots(self):
        assert_zero(self, smoke)

    def test_footage_explicit_reporting(self):
        assert_reporting(self, smoke)

    def test_footage_navigation_mounts_and_cadence(self):
        assert_cadence(self, smoke, self.base)


    def test_capture_helpers_are_byte_identical_to_baseline(self):
        import inspect
        # SHA256 of each verbatim helper's source in 907e070. This also stops
        # the fixture's shared helpers from masking a change on both sides.
        expected = {
            '_capture_with_clip': 'd45fa9c745139be19a88bf25b1b4c5532c6b2ef8b0b706d5c96fdcf9647a78dc',
            '_safe_panel_screenshot': '8bf877c6d4628b1988274e57e59f1d61eec8d45c5f2af7ba16ecd881274a1fe2',
            '_capture_at_offset': '8b583c14122e4d99b55431daebc462e5988cdcf8b643284d00e34840caa56627',
            '_panel_clip_and_dims': '3fb525c0ea0c278d588c54502bd3dc711f626b2c861f76c880b80615d54f715f'}
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256(inspect.getsource(
                getattr(smoke, name)).encode()).hexdigest(), digest, name)

    def test_incompatible_options_refuse_before_opening_browser(self):
        for options in ({'diag_data': True}, {'inject_blink_ms': 400}):
            with self.assertRaisesRegex(ValueError, 'footage mode cannot evaluate'):
                smoke.run_tab(None, 'overview', 'http://fixture/', '.', 6, 60,
                              smoke.lib.BLINK_THRESHOLD, footage_only=True, **options)

    def test_runtime_failure_still_reports_unevaluated_checks(self):
        evaluate = Page.evaluate

        def fail_render(page, js, arg=None):
            if js in smoke.PANEL_CHECKS.values():
                raise smoke.SmokeFailure('deliberate render failure')
            return evaluate(page, js, arg)

        with patch.object(Page, 'evaluate', fail_render), \
                contextlib.redirect_stderr(io.StringIO()):
            page, result = drive(smoke, 'timeline', True)
        self.assertEqual(calls(page, 'screenshot'), [])
        self.assertFalse(result['ok'])
        self.assertFalse(result['footage_complete'])
        self.assertEqual(result['ticks_observed'], 0)
        self.assertEqual(result['error'], 'deliberate render failure')
        for key in ('no_blink', 'frame_dims', 'frame_spacing', 'capture_budget'):
            self.assertIsNone(result[key]['ok'])
            self.assertEqual(result[key]['detail'], NOTE)

    def test_cli_reports_artifact_completion_without_a_green_gate(self):
        browser = Browser('overview')
        driver = contextlib.nullcontext(types.SimpleNamespace(
            chromium=types.SimpleNamespace(launch=lambda: browser)))
        with tempfile.TemporaryDirectory(dir=ROOT / 'tests') as out:
            argv = ['ui_live_smoke.py', '--url', 'http://fixture/', '--out-dir',
                    str(Path(out) / 'footage'), '--footage-only', '--tabs',
                    'overview', '--device-scale-factor', '1']
            stdout = io.StringIO()
            with patch.object(sys, 'argv', argv), \
                    patch.object(smoke, '_require_playwright'), \
                    patch.object(smoke, 'sync_playwright', lambda: driver), \
                    patch.object(smoke.time, 'monotonic', lambda: browser.page.now / 1000), \
                    contextlib.redirect_stdout(stdout):
                self.assertEqual(smoke.main(), 0)
            result = json.loads((Path(out) / 'footage/summary.json').read_text())
        self.assertFalse(result['ok'])
        self.assertTrue(result['footage_complete'])
        self.assertEqual(result['detail'], NOTE)
        self.assertNotIn('PASS', stdout.getvalue())
        self.assertIn(NOTE, stdout.getvalue())
        self.assertEqual(calls(browser.page, 'screenshot'), [])

    def test_missing_video_cannot_report_footage_recorded(self):
        new_context = Browser.new_context

        def no_video(browser, **kwargs):
            context = new_context(browser, **kwargs)
            browser.page.video = None
            return context

        with patch.object(Browser, 'new_context', no_video):
            _, result = drive(smoke, 'overview', True)
        self.assertFalse(result['footage_complete'])
        self.assertFalse(result['ok'])

    def test_browser_timing_statistic_detects_a_capture_cost_change(self):
        from footage_capture_gate import timing_report
        samples = [400 + (i % 5) * 3 for i in range(18)]
        self.assertTrue(timing_report(samples, samples)['ok'])
        self.assertFalse(timing_report(samples, [x + 60 for x in samples])['ok'])

    def test_cli_refuses_gating_directory_before_reset(self):
        for extra in ([], ['--out-dir', smoke.RESULTS_DIR],
                      ['--out-dir', smoke.RESULTS_DIR + '/child'],
                      ['--out-dir', str(Path(smoke.RESULTS_DIR).parent)],
                      ['--out-dir', '/unused', '--diag-data'],
                      ['--out-dir', '/unused', '--inject-blink-ms', '400']):
            with patch.object(sys, 'argv', ['ui_live_smoke.py', '--mock',
                                           '--footage-only', *extra]), \
                    patch.object(smoke, 'start_mock_server') as start, \
                    patch.object(smoke.lib, 'reset_output_dir') as reset, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    smoke.main()
                self.assertEqual(error.exception.code, 2)
                start.assert_not_called()
                reset.assert_not_called()


def mutations():
    """Mutate executable source in memory; each real guard must fail, then pass.

    No checkout files are modified. An unexpected exception is a harness error,
    never counted as a successful negative control.
    """
    source = Path(smoke.__file__).read_text()
    base = baseline()
    test = unittest.TestCase()
    controls = [
        ("default count: duplicate each capture",
         'return page.screenshot(clip=clip, scale="css")',
         'page.screenshot(clip=clip, scale="css")\n        return page.screenshot(clip=clip, scale="css")',
         lambda m: assert_normal(test, m, base)),
        ("default timing: add 60ms to every capture",
         '    frame = _capture_with_clip(page, clip)',
         '    page.wait_for_timeout(60)\n    frame = _capture_with_clip(page, clip)',
         lambda m: assert_normal(test, m, base)),
        ("A: leave exactly one screenshot live in footage",
         '                    _wait_footage_offset(page, mount_at_ms, offset)',
         '                    if i == 1 and frame_index == 0:\n'
         '                        page.screenshot(clip=sweep_clip, scale="css")\n'
         '                    _wait_footage_offset(page, mount_at_ms, offset)',
         lambda m: assert_zero(test, m)),
        ("B: silently skip frame_dims without its report",
         '        result[key]["detail"] = FOOTAGE_NOT_EVALUATED',
         '        result[key]["detail"] = (None if key == "frame_dims" else FOOTAGE_NOT_EVALUATED)',
         lambda m: assert_reporting(test, m)),
        ("C: wait for every second live tick in footage",
         '_wait_for_tick(page, tick_baseline + i, TICK_TIMEOUT_S)',
         '_wait_for_tick(page, tick_baseline + (2 * i if footage_only else i), TICK_TIMEOUT_S)',
         lambda m: assert_cadence(test, m, base)),
    ]
    for name, old, new, guard in controls:
        if source.count(old) != 1:
            raise AssertionError(f'{name}: mutation site is absent or ambiguous')
        try:
            guard(load_source(source.replace(old, new)))
        except AssertionError as error:
            print(f'RED [{name}]: {error}')
        else:
            raise AssertionError(f'NEGATIVE CONTROL SURVIVED: {name}')
        guard(smoke)
        print(f'GREEN [{name}]: restored production source')
    print('5/5 mutations rejected; 5/5 restored guards passed')


if __name__ == '__main__':
    if sys.argv[1:] == ['--mutations']:
        mutations()
    else:
        unittest.main()
