#!/usr/bin/env python3
"""#758: first-info recovery over a live, silent WebSocket (no timeout override)."""
import json
import unittest

from free_ports import find_free_base
from mock_server import handle_request
from playwright.sync_api import sync_playwright, expect
from test_web_ui import app_url, start_mock_server, stop_mock_server


class InfoRetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch(headless=True)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.context = self.browser.new_context()
        self.page = self.context.new_page()
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.sockets = []
        self.infos = []
        self.page.on('websocket', self.record_socket)
        self.mock = None

    def record_socket(self, socket):
        self.sockets.append(socket)

        def record(raw):
            msg = json.loads(raw)
            if msg.get('cmd') == 'info':
                self.infos.append(msg)
        socket.on('framesent', record)

    def tearDown(self):
        self.context.close()
        if self.mock:
            stop_mock_server(self.mock)
        self.assertEqual(self.errors, [])

    def start(self, delay_ms=0, delay_count=1, clock=False):
        port = find_free_base(2)
        self.mock = start_mock_server(port=port, extra_env={
            'PGWT_MOCK_INFO_DELAY_MS': str(delay_ms),
            'PGWT_MOCK_INFO_DELAY_COUNT': str(delay_count),
        })
        if clock:
            self.page.clock.install()
        self.page.goto(app_url(port))
        if delay_ms:
            expect(self.page.locator('#status')).to_have_text('Loading...')
        self.page.wait_for_function("() => document.querySelector('#status') !== null")

    def test_recovers_after_first_info_exceeds_real_30s_bound(self):
        self.start(delay_ms=35000)
        # This is the regression oracle: today's terminal Error: timeout
        # cannot reach connected even when the late reply eventually arrives.
        expect(self.page.locator('#status')).to_have_class('status connected', timeout=42000)
        self.assertEqual(len(self.sockets), 1, 'recover on the original socket')
        self.assertEqual(len(self.infos), 2, 'redispatch info with a fresh id')
        self.assertNotEqual(self.infos[0]['id'], self.infos[1]['id'])
        expect(self.page.locator('#table-container tbody tr').first).to_be_visible()
        print('PASS: silent first info >30s -> connected on same socket, fresh info id')

    def test_fast_path_sends_one_info_without_backoff(self):
        self.start()
        expect(self.page.locator('#status')).to_have_class('status connected', timeout=5000)
        self.assertEqual(len(self.infos), 1)
        expect(self.page.locator('#table-container tbody tr').first).to_be_visible()
        print('PASS: fast path -> connected with one info and populated table')

    def test_bridge_timeout_envelope_recovers(self):
        infos = []

        def route(socket):
            def respond(raw):
                msg = json.loads(raw)
                if msg.get('cmd') == 'info':
                    infos.append(msg)
                    if len(infos) == 1:
                        socket.send(json.dumps({
                            'id': msg['id'], 'error': 'timeout', 'transport': True,
                        }))
                        return
                socket.send(json.dumps(handle_request(msg)))
            socket.on_message(respond)
        self.page.route_web_socket('**/ws', route)
        self.start()
        expect(self.page.locator('#status')).to_have_class('status connected', timeout=5000)
        self.assertEqual(len(infos), 2)

    def test_command_error_is_not_retried(self):
        infos = []

        def route(socket):
            def respond(raw):
                msg = json.loads(raw)
                infos.append(msg)
                socket.send(json.dumps({'id': msg['id'], 'error': 'info refused'}))
            socket.on_message(respond)
        self.page.route_web_socket('**/ws', route)
        self.start(clock=True)
        expect(self.page.locator('#status.error')).to_have_text('Error: info refused')
        self.page.clock.fast_forward(120000)
        self.assertEqual(len(infos), 1)

    def test_disconnect_during_backoff_leaves_reconnect_in_charge(self):
        self.page.add_init_script('''
            const NativeWebSocket = window.WebSocket;
            window.WebSocket = class extends NativeWebSocket {
                constructor(...args) { super(...args); window.testSocket = this; }
            };
        ''')
        self.start(delay_ms=120000, delay_count=-1, clock=True)
        self.page.clock.fast_forward(30000)
        expect(self.page.locator('#status.connecting')).to_contain_text('Retrying info')
        self.page.evaluate('window.testSocket.close()')
        expect(self.page.locator('#status.error')).to_have_text('Reconnecting in 2s...')
        self.page.clock.fast_forward(1000)
        expect(self.page.locator('#status.error')).to_have_text('Reconnecting in 2s...')
        with self.page.expect_websocket():
            self.page.clock.fast_forward(1000)
        expect(self.page.locator('#status.connecting')).to_have_text('Loading...')
        self.assertEqual(len(self.sockets), 2)
        self.assertEqual(len(self.infos), 2)

    def test_exhaustion_shows_error_and_stops_retrying(self):
        self.start(delay_ms=120000, delay_count=-1, clock=True)
        for attempt, delay in ((1, 1000), (2, 2000)):
            self.page.clock.fast_forward(30000)
            expect(self.page.locator('#status.connecting')).to_contain_text('Retrying info')
            self.assertEqual(len(self.infos), attempt)
            with self.sockets[0].expect_event('framesent'):
                self.page.clock.fast_forward(delay)
            expect(self.page.locator('#status.connecting')).to_contain_text(
                f'attempt {attempt + 1}/3')
            self.assertEqual(len(self.infos), attempt + 1)
        self.page.clock.fast_forward(30000)
        expect(self.page.locator('#status.error')).to_have_text('Error: timeout')
        self.page.clock.fast_forward(120000)
        self.assertEqual(len(self.infos), 3)
        print('PASS: three attempts, 1s/2s backoff, then stable Error: timeout')


if __name__ == '__main__':
    unittest.main(verbosity=2)
