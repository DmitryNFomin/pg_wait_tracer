#!/usr/bin/env python3
"""Failure diagnostics through the real run_tab and navigation, no browser needed.

--probe prints the navigation regression's evidence before asserting it, so
the same command can demonstrate the old code losing the sentinels.
--snapshot-js checks the actual snapshot expression with Node (local tier).
"""
import asyncio
import contextlib
import copy
import io
import json
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch

import ui_live_smoke as smoke


CONSOLE = 'console: diagnostic-console-sentinel-544'
SNAPSHOT = {
    'status': 'recorded',
    'active_tab': {'status': 'recorded', 'value': 'diagnostic-active-sentinel-544'},
    'last_mount': {'status': 'recorded', 'value': {
        'id': 'diagnostic-mount-sentinel-544', 'seq': 7, 'at': 1234}},
    'pane_errors': {'status': 'recorded', 'value': ['diagnostic-pane-sentinel-544']},
    'pending_requests': {'status': 'recorded', 'value': [
        {'id': '42', 'cmd': 'diagnostic-pending-sentinel-544', 'channel': 'overview'}]},
}
PRE_MOUNT = {'tick': 1, 'note': 'diagnostic-pre-mount-sentinel-544'}


class Page:
    video = None

    def __init__(self, failure='navigation', snapshot_mode='recorded'):
        self.failure = failure
        self.snapshot_mode = snapshot_mode
        self.handlers = {}
        self.closed = False
        self.reads = 0
        self.cancelled = False
        self._impl_obj = types.SimpleNamespace(evaluate=self.read_snapshot)

    def on(self, event, handler):
        self.handlers[event] = handler

    def emit_error(self):
        self.handlers['console'](types.SimpleNamespace(
            type='error', text=CONSOLE.removeprefix('console: ')))

    def goto(self, url):
        pass

    def evaluate(self, js):
        if 'window.innerWidth' in js:
            return {'w': 1280, 'h': 900, 'dpr': 1}
        if js == smoke.TICK_HOOK_JS:
            return None
        if js == 'Date.now()':
            return 1000
        raise AssertionError(f'unmodelled evaluate: {js}')

    def query_selector(self, selector):
        return self

    def click(self, selector):
        # The real _navigate_to_tab has reached its tab click.
        self.emit_error()
        if self.failure == 'exception':
            raise RuntimeError('navigation exception sentinel')

    def wait_for_selector(self, selector, **kwargs):
        if selector != '#status.connected':
            raise smoke.PWTimeoutError('forced navigation failure')

    def _sync(self, coroutine):
        return asyncio.run(coroutine)

    async def read_snapshot(self, js):
        assert not self.closed, 'snapshot must precede context.close'
        assert js == smoke.FAILURE_SNAPSHOT_JS
        self.reads += 1
        if self.snapshot_mode == 'throws':
            raise RuntimeError('snapshot read sentinel')
        if self.snapshot_mode == 'wedged':
            try:
                await asyncio.Event().wait()
            finally:
                self.cancelled = True
        # Console events can also arrive while the read pumps Playwright.
        self.handlers['pageerror']('diagnostic-pageerror-sentinel-544')
        return copy.deepcopy(SNAPSHOT)


class Browser:
    def __init__(self, page):
        self.page = page

    def new_context(self, **kwargs):
        return self

    def add_init_script(self, script):
        pass

    def new_page(self):
        return self.page

    def close(self):
        self.page.closed = True


def drive(page):
    with tempfile.TemporaryDirectory() as out, \
            contextlib.redirect_stderr(io.StringIO()):
        return smoke.run_tab(Browser(page), 'overview', 'http://fixture/', out,
                             6, 0.01, smoke.lib.BLINK_THRESHOLD)


def assert_navigation_evidence(test, result):
    test.assertIn(CONSOLE, result['clean']['console_errors'])
    test.assertIn('diagnostic-pageerror-sentinel-544',
                  '\n'.join(result['clean']['console_errors']))
    test.assertEqual(result['failure_snapshot'], SNAPSHOT)
    # Navigation never reached the Timeline sweep: absence is honest;
    # a constant empty list pretending to be a measurement is not.
    test.assertNotIn('pre_mount_diagnostic', result)
    test.assertIsNone(result['ttfp_ms'])


class FailureTests(unittest.TestCase):
    def test_navigation_preserves_console_and_snapshot_contents(self):
        page = Page()
        result = drive(page)
        self.assertIn('panel did not render', result['error'])
        assert_navigation_evidence(self, result)
        self.assertEqual(page.reads, 1)
        self.assertTrue(page.closed)

    def test_unexpected_exception_preserves_the_same_evidence(self):
        result = drive(Page(failure='exception'))
        self.assertIn('unhandled exception', result['error'])
        assert_navigation_evidence(self, result)

    def test_snapshot_exception_cannot_replace_original_failure(self):
        result = drive(Page(snapshot_mode='throws'))
        self.assertIn('panel did not render', result['error'])
        self.assertIn(CONSOLE, result['clean']['console_errors'])
        self.assertEqual(result['failure_snapshot']['status'], 'unavailable')
        self.assertIn('snapshot read sentinel', result['failure_snapshot']['error'])

    def test_wedged_snapshot_is_bounded_and_console_is_still_preserved(self):
        page = Page(snapshot_mode='wedged')
        start = time.monotonic()
        with patch.object(smoke, 'FAILURE_SNAPSHOT_TIMEOUT_S', 0.03):
            result = drive(page)
        self.assertLess(time.monotonic() - start, 1)
        self.assertTrue(page.cancelled)
        self.assertTrue(page.closed)
        self.assertIn('panel did not render', result['error'])
        self.assertIn(CONSOLE, result['clean']['console_errors'])
        self.assertEqual(result['failure_snapshot']['status'], 'unavailable')
        self.assertIn('timed out', result['failure_snapshot']['error'])

    def test_unavailable_playwright_bridge_is_explicit(self):
        page = Page()
        del page._impl_obj
        result = drive(page)
        self.assertIn(CONSOLE, result['clean']['console_errors'])
        self.assertEqual(result['failure_snapshot']['status'], 'unavailable')
        self.assertTrue(result['failure_snapshot']['error'])

    def test_failed_builder_retains_collected_pre_mount_content(self):
        result = smoke.lib.build_failed_tab_result(
            'timeline', 'later failure', console_errors=[CONSOLE],
            pre_mount_diagnostics=[PRE_MOUNT])
        self.assertEqual(result['clean']['console_errors'], [CONSOLE])
        self.assertEqual(result['pre_mount_diagnostic'], [PRE_MOUNT])

    def test_failure_keeps_all_console_errors_and_pgwt_classification(self):
        page = Page()
        emit = page.emit_error

        def emit_many():
            for index in range(15):
                page.handlers['console'](types.SimpleNamespace(
                    type='error', text=f'error-{index}'))
            page.handlers['console'](types.SimpleNamespace(
                type='error', text='[pgwt] diagnostic-pgwt-sentinel-544'))
            emit()

        page.emit_error = emit_many
        result = drive(page)
        self.assertEqual(result['clean']['console_errors'][:15],
                         [f'console: error-{index}' for index in range(15)])
        assert_navigation_evidence(self, result)
        self.assertEqual(result['clean']['pgwt_errors'],
                         ['console: [pgwt] diagnostic-pgwt-sentinel-544'])

    def test_later_failure_retains_already_drained_and_pre_mount_content(self):
        from test_ui_live_smoke_footage import Browser as TickBrowser, Page as TickPage
        browser = TickBrowser('timeline')
        evaluate = TickPage.evaluate
        wait_tick = smoke._wait_for_tick
        waits = 0

        def on(page, event, handler):
            if not hasattr(page, 'handlers'):
                page.handlers = {}
            page.handlers[event] = handler

        def read(page, js, arg=None):
            if js == smoke.LEGEND_COLORS_JS:
                page.handlers['console'](types.SimpleNamespace(
                    type='error', text=CONSOLE.removeprefix('console: ')))
            return evaluate(page, js, arg)

        def next_tick(*args):
            nonlocal waits
            waits += 1
            if waits == 2:
                raise smoke.SmokeFailure('later-tick-failure-sentinel')
            return wait_tick(*args)

        with tempfile.TemporaryDirectory() as out, \
                patch.object(TickPage, 'on', on), \
                patch.object(TickPage, 'evaluate', read), \
                patch.object(smoke, '_wait_for_tick', next_tick), \
                patch.object(smoke.lib, 'pre_mount_diagnostic_verdict',
                             return_value=PRE_MOUNT.copy()), \
                contextlib.redirect_stderr(io.StringIO()):
            result = smoke.run_tab(browser, 'timeline', 'http://fixture/', out,
                                   6, 60, smoke.lib.BLINK_THRESHOLD)
        self.assertEqual(result['error'], 'later-tick-failure-sentinel')
        self.assertEqual(result['clean']['console_errors'], [CONSOLE])
        self.assertEqual(result['pre_mount_diagnostic'], [PRE_MOUNT])
        self.assertEqual(result['ttfp_ms'], 0)


def check_snapshot_js():
    """Execute the production expression, including missing/throwing hooks.

    Kept in the Node-equipped local check tier; the listed Python unit tests
    need neither Node nor a browser. No replacement snapshot implementation.
    """
    harness = """
const expected = EXPECTED;
globalThis.window = {__pgwt: {
    activeTab: expected.active_tab.value,
    viewMount: () => expected.last_mount.value,
    transport: {pending: {42: expected.pending_requests.value[0]}},
}};
globalThis.document = {
    querySelectorAll: selector => {
        if (selector !== '.pane-error') throw new Error('wrong pane selector');
        return expected.pane_errors.value.map(textContent => ({textContent}));
    },
};
const read = EXPRESSION;
assert.deepStrictEqual(read(), expected);
delete window.__pgwt;
let result = read();
for (const field of ['active_tab', 'last_mount', 'pending_requests']) {
    assert.equal(result[field].status, 'unavailable');
    assert.ok(result[field].error);
}
// An actual empty DOM read is explicitly recorded, not a literal [].
document.querySelectorAll = () => [];
assert.deepStrictEqual(read().pane_errors, {status: 'recorded', value: []});
window.__pgwt = {
    get activeTab() { throw new Error('active-read-sentinel'); },
    viewMount: () => { throw new Error('mount-read-sentinel'); },
    transport: {get pending() { throw new Error('pending-read-sentinel'); }},
};
document.querySelectorAll = () => { throw new Error('pane-read-sentinel'); };
result = read();
for (const [field, sentinel] of Object.entries({active_tab: 'active',
        last_mount: 'mount', pane_errors: 'pane', pending_requests: 'pending'})) {
    assert.equal(result[field].status, 'unavailable');
    assert.ok(result[field].error.includes(sentinel + '-read-sentinel'));
}
console.log('snapshot JavaScript: all four sentinels retained; missing/throwing reads explicit');
"""
    source = "const assert = require('node:assert/strict');\n" + harness.replace(
        'EXPECTED', json.dumps(SNAPSHOT)).replace('EXPRESSION', smoke.FAILURE_SNAPSHOT_JS)
    subprocess.run(['node'], input=source, text=True, check=True, timeout=5)


if __name__ == '__main__':
    if sys.argv[1:] == ['--probe']:
        result = drive(Page())
        print(json.dumps({
            'console_errors': result['clean']['console_errors'],
            'failure_snapshot': result.get('failure_snapshot', 'ABSENT'),
            'pre_mount_diagnostic': result.get('pre_mount_diagnostic', 'ABSENT'),
        }, sort_keys=True), flush=True)
        assert_navigation_evidence(unittest.TestCase(), result)
    elif sys.argv[1:] == ['--snapshot-js']:
        check_snapshot_js()
    else:
        unittest.main()
