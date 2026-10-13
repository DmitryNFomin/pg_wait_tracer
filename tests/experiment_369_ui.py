#!/usr/bin/env python3
"""Replay #369 captured JSON through each revision's actual UI in Chromium.

python3 tests/experiment_369_ui.py EVIDENCE_DIRECTORY
The mock supplies unrelated app commands; only Sessions responses are replayed.
No UI source is modified. Screenshots and DOM observations are saved as evidence.
"""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

from playwright.sync_api import sync_playwright


def load(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def main():
    evidence = Path(sys.argv[1]).resolve()
    observations = []
    with tempfile.TemporaryDirectory(prefix='pgwt-369-ui-') as tmp, sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        for label, ref in [('master', '8a52a51'), ('d231d18', 'd231d18')]:
            root = Path(tmp) / label
            root.mkdir()
            archive = root / 'source.tar'
            subprocess.run(['git', 'archive', '-o', str(archive), ref], check=True)
            with tarfile.open(archive) as tar:
                tar.extractall(root, filter='data')
            # Pick adjacent free ports because mock HTTP and WS use port, port+1.
            for port in range(19850, 19950, 2):
                try:
                    with socket.socket() as a, socket.socket() as b:
                        a.bind(('127.0.0.1', port))
                        b.bind(('127.0.0.1', port+1))
                    break
                except OSError:
                    continue
            log = (evidence / f'{label}.ui-mock.log').open('w')
            mock = subprocess.Popen([sys.executable, str(root / 'tests/mock_server.py'),
                                     '--port', str(port)], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1', port+1), timeout=.2):
                            break
                    except OSError:
                        if mock.poll() is not None:
                            raise RuntimeError('mock failed; see log')
                        time.sleep(.1)
                production = load(evidence / f'{label}.production.responses.jsonl')
                forced = load(evidence / f'{label}.forced-summary.responses.jsonl')
                cases = [('production-class', production[2]),
                         ('production-cpu', production[5]),
                         ('production-invalid-class', production[7]),
                         ('production-empty-window', production[9]),
                         ('forced-summary-class', forced[0])]
                for name, response in cases:
                    page = browser.new_page(viewport={'width': 1280, 'height': 900})
                    seen = []
                    def intercept(route):
                        server = route.connect_to_server()
                        def receive(message):
                            request = json.loads(message)
                            if request.get('cmd') == 'top_sessions':
                                seen.append(request)
                                route.send(json.dumps(dict(response, id=request['id'])))
                            else:
                                server.send(message)
                        route.on_message(receive)
                    page.route_web_socket('**/ws', intercept)
                    page.goto(f'http://127.0.0.1:{port}/?ws=ws://127.0.0.1:{port+1}/ws')
                    page.wait_for_selector('#status.connected', timeout=15000)
                    page.click('.tab[data-tab="sessions"]')
                    if response.get('error'):
                        page.wait_for_selector('#table-container > .pane-error', timeout=10000)
                    elif response.get('rows'):
                        page.wait_for_selector('#table-container tbody tr', timeout=10000)
                    else:
                        page.wait_for_function("document.querySelector('#table-container').textContent.includes('No data for selected range')")
                    observation = page.locator('#table-container').evaluate('''el => ({
                        text: el.textContent.trim(), html: el.innerHTML,
                        rows: el.querySelectorAll('tbody tr').length,
                        error_cards: el.querySelectorAll('.pane-error').length,
                        retry_buttons: el.querySelectorAll('.pane-error-retry').length,
                        empty_placeholder: el.textContent.includes('No data for selected range'),
                    })''')
                    assert seen, 'Sessions response was never requested'
                    if response.get('error'):
                        assert observation['error_cards'] == 1
                        assert observation['retry_buttons'] == 1
                        assert not observation['empty_placeholder']
                        assert 'compute_failed' in observation['text']
                    elif response.get('rows'):
                        assert observation['rows'] > 0 and observation['error_cards'] == 0
                    else:
                        assert observation['empty_placeholder'] and observation['error_cards'] == 0
                    observation.update(revision=label, case=name, replayed_response=response,
                                       sessions_requests=seen)
                    observations.append(observation)
                    page.screenshot(path=str(evidence / f'{label}.{name}.png'), full_page=True)
                    page.close()
                    print(label, name, {k: observation[k] for k in
                          ['rows', 'error_cards', 'empty_placeholder']}, flush=True)
            finally:
                mock.terminate()
                mock.wait(timeout=5)
                log.close()
        browser.close()
    (evidence / 'ui-observations.json').write_text(json.dumps(observations, indent=2) + '\n')


if __name__ == '__main__':
    main()
