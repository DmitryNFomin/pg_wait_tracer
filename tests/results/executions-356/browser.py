"""Complete warm Waterfall paint; no time_model requests or inferred client share."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from playwright.sync_api import sync_playwright
root = Path(sys.argv[1])
trace = sys.argv[2]
arm = sys.argv[3]
out = root / 'evidence'
# This is the same fixed 900-second replay window as the endpoint run.
info = subprocess.run([str(root/arm/'pgwt-server'), trace], input='{"id":1,"cmd":"info"}\n',
                      text=True, capture_output=True, check=True)
end = json.loads(info.stdout)['to_ns']
start = end-900_000_000_000
server_log = out / f'browser-{arm}.stderr'
server_log.write_text('')
wrapper = out / f'browser-{arm}.sh'
wrapper.write_text('#!/usr/bin/env bash\n'
    f'echo "EXSERVER pid=$$" >> {server_log}\n'
    f'exec env PGWT_CURRENT_TRACE_CACHE_STATS=1 {root/arm/"pgwt-server"} "$@" 2>> {server_log}\n')
wrapper.chmod(0o755)
bridge = subprocess.Popen([str(root/'before/web/pgwt'), '--port', '8496', '--trace-dir', trace,
    '--server-path', str(wrapper), 'root@localhost'], stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT, text=True, start_new_session=True)
context = None
try:
    # Wait for the bridge's startup event, without retrying or sleeping.
    with (out / f'bridge-{arm}.log').open('w') as log:
        for line in bridge.stdout:
            log.write(line); log.flush()
            if 'Open http://' in line:
                break
        else:
            raise RuntimeError('bridge exited before startup')
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            context = browser.new_context(viewport={'width':1710,'height':981}, device_scale_factor=2)
            page = context.new_page()
            page.set_default_timeout(60000)
            wire, errors = [], []
            def sent(raw):
                req = json.loads(raw)
                assert req['cmd'] != 'time_model', req
                wire.append(req)
            def socket(ws):
                ws.on('framesent', sent)
            page.on('websocket', socket)
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.on('requestfailed', lambda r: errors.append(r.url+': '+str(r.failure)))
            page.on('console', lambda m: errors.append(m.text) if m.type=='error' else None)
            page.goto(f'http://localhost:8496/#tab=queries&from={start}&to={end}')
            try:
                page.wait_for_function("window.__pgwt && window.__pgwt.viewMount() && window.__pgwt.viewMount().id === 'queries'")
            except Exception:
                (out/f'browser-{arm}-failure.json').write_text(json.dumps(dict(errors=errors, wire=wire, html=page.content()), indent=2))
                raise
            for i in range(7):
                page.wait_for_function('Object.keys(window.__pgwt.transport.pending).length === 0')
                before = page.evaluate('window.__pgwt.viewMount()')
                began = page.evaluate('Date.now()')
                page.click(".tab[data-tab='waterfall']")
                page.wait_for_function("seq => {const m=window.__pgwt.viewMount(); return m && m.id==='waterfall' && m.seq>seq;}", arg=before['seq'])
                mounted = page.evaluate('window.__pgwt.viewMount()')
                assert page.query_selector('#waterfall-chart canvas') is not None
                state = page.evaluate('({from:window.__pgwt.timeRange.from,to:window.__pgwt.timeRange.to,live:document.getElementById("live-btn").classList.contains("active")})')
                # JS represents nanoseconds as doubles; pin the exact JS inputs.
                assert page.evaluate('([start,end]) => window.__pgwt.timeRange.from === Number(start) && window.__pgwt.timeRange.to === Number(end) && window.__pgwt.timeRange.to-window.__pgwt.timeRange.from === 900000000000', arg=[str(start),str(end)]) and not state['live'], state
                assert not errors, errors
                print(json.dumps(dict(arm=arm, call=i, excluded_cold=i==0, ttfp_ms=mounted['at']-began, state=state)), flush=True)
                before = mounted
                page.click(".tab[data-tab='queries']")
                page.wait_for_function("seq => {const m=window.__pgwt.viewMount(); return m && m.id==='queries' && m.seq>seq;}", arg=before['seq'])
            (out/f'browser-{arm}-requests.json').write_text(json.dumps(wire, indent=2)+'\n')
            context.close(); context = None
            browser.close()
finally:
    os.killpg(bridge.pid, signal.SIGTERM)
    bridge.wait(timeout=10)
assert server_log.read_text().count('EXSERVER pid=') == 1, 'server restarted'
