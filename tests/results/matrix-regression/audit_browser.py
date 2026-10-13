"""Same browser build/box/trace/window, warm navigations in one upstream process.

This is a targeted replay measurement, not the live smoke or full check suite.
The initial deep link bypasses Overview, so time_model at 900 s is never sent.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import urllib.request

from playwright.sync_api import sync_playwright

ROOT = Path('/root/cx-matrix-regression')
OUT = ROOT / 'audit'
START, END = 1791800864284323682, 1791801764284323682
TABS = ['queries', 'transitions', 'concurrency', 'waterfall', 'scatter', 'matrix']
PORT = 8497

with sync_playwright() as pw:
    browser = pw.chromium.launch()
    try:
        for rep in range(2):
            for arm in ['pre', 'aggregate', 'final'] if rep == 0 else ['final', 'aggregate', 'pre']:
                server_log = OUT / f'browser-server-{arm}-{rep}.stderr'
                server_log.write_text('')
                wrapper = OUT / f'server-{arm}-{rep}.sh'
                wrapper.write_text('#!/usr/bin/env bash\n'
                    f'echo "MXSERVER pid=$$" >> {server_log}\n'
                    f'exec env PGWT_CURRENT_TRACE_CACHE_STATS=1 PGWT_TRANSITIONS_PROVENANCE=1 '
                    f'{ROOT / arm / "pgwt-server"} "$@" 2>> {server_log}\n')
                wrapper.chmod(0o755)
                with (OUT / f'bridge-{arm}-{rep}.log').open('w') as bridge_log:
                    bridge = subprocess.Popen([str(ROOT / 'final/web/pgwt'), '--port', str(PORT),
                        '--trace-dir', '/tmp/pgwt_cx_rehearsal_tqWPfm', '--server-path', str(wrapper),
                        'root@localhost'], stdout=bridge_log, stderr=bridge_log, start_new_session=True)
                    context = None
                    try:
                        for _ in range(100):
                            assert bridge.poll() is None, 'bridge exited'
                            try:
                                urllib.request.urlopen(f'http://localhost:{PORT}/session', timeout=1).close()
                                break
                            except OSError:
                                time.sleep(0.1)
                        else:
                            raise RuntimeError('bridge did not start')
                        context = browser.new_context(viewport={'width': 1710, 'height': 981}, device_scale_factor=2)
                        page = context.new_page()
                        page.set_default_timeout(60000)
                        pending, wire, errors = {}, [], []

                        def on_sent(payload):
                            req = json.loads(payload)
                            assert req['cmd'] != 'time_model', req
                            pending[req['id']] = (time.perf_counter(), req)
                            wire.append(dict(kind='request', request=req))

                        def on_received(payload):
                            data = json.loads(payload)
                            if data['id'] in pending:
                                started, req = pending.pop(data['id'])
                                wire.append(dict(kind='response', request=req,
                                    rtt_ms=round((time.perf_counter()-started)*1000, 3),
                                    bytes=len(payload), error=data.get('error'), total=data.get('total'),
                                    merged_blocks=data.get('merged_blocks'), decoded_blocks=data.get('decoded_blocks')))
                                if req['cmd'] in ('top_queries', 'transitions', 'executions',
                                                  'execution_detail', 'exec_scatter', 'concurrency'):
                                    assert not data.get('error') and not data.get('unavailable'), data

                        def on_socket(socket):
                            socket.on('framesent', on_sent)
                            socket.on('framereceived', on_received)

                        page.on('websocket', on_socket)
                        page.on('pageerror', lambda error: errors.append(str(error)))
                        page.goto(f'http://localhost:{PORT}/#tab=queries&from={START}&to={END}')
                        page.wait_for_function("window.__pgwt && window.__pgwt.viewMount() && window.__pgwt.viewMount().id === 'queries'")
                        for cycle in range(4):
                            for tab in TABS:
                                page.wait_for_function('Object.keys(window.__pgwt.transport.pending).length === 0')
                                before = page.evaluate('window.__pgwt.viewMount()')
                                wire_start = len(wire)
                                started = page.evaluate('Date.now()')
                                page.click(f".tab[data-tab='{tab}']")
                                page.wait_for_function("([tab, seq]) => { const m=window.__pgwt.viewMount(); return m && m.id===tab && m.seq>seq; }", arg=[tab, before['seq']])
                                mounted = page.evaluate('window.__pgwt.viewMount()')
                                selector = {'queries': '#table-container table tbody tr',
                                    'transitions': '#dfg-container canvas, #dfg-container svg',
                                    'concurrency': '#concurrency-chart canvas',
                                    'waterfall': '#waterfall-chart canvas',
                                    'scatter': '#scatter-chart canvas',
                                    'matrix': '#matrix-chart canvas'}[tab]
                                assert page.query_selector(selector) is not None, ('no rendered data', arm, tab)
                                state = page.evaluate('({from:window.__pgwt.timeRange.from,to:window.__pgwt.timeRange.to,live:document.getElementById("live-btn").classList.contains("active")})')
                                assert not state['live'] and abs(state['from']-START) < 256 and abs(state['to']-END) < 256, state
                                assert not errors, errors
                                row = dict(arm=arm, rep=rep, cycle=cycle, tab=tab,
                                    excluded_cold=cycle == 0, ttfp_ms=mounted['at']-started,
                                    state=state, wire=wire[wire_start:])
                                print(json.dumps(row), flush=True)
                        (OUT / f'wire-{arm}-{rep}.json').write_text(json.dumps(wire, indent=2)+'\n')
                    finally:
                        if context:
                            context.close()
                        os.killpg(bridge.pid, signal.SIGTERM)
                        bridge.wait(timeout=10)
                assert server_log.read_text().count('MXSERVER pid=') == 1, 'upstream restarted'
    finally:
        browser.close()
