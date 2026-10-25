#!/usr/bin/env python3
"""Real replay -> pgwt-server -> Go SSH bridge -> Chromium verification.

Run on the gate box via replay_ui.sh (holds the shared box-check lock).
No live daemon or workload. Outputs, including raw WS frames, belong outside
this checkout. Agreement tests presentation, not absolute accounting.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent


def trace_headers(directory):
    """Same uncompressed 28-byte header on current and rotated .trace.lz4."""
    headers = []
    paths = list(Path(directory).glob('*.trace.lz4'))
    current = Path(directory) / 'current.trace'
    if current.exists():
        paths.append(current)
    for path in paths:
        with path.open('rb') as stream:
            raw = stream.read(28)
        if len(raw) != 28:
            raise ValueError(f'unreadable trace header: {path}')
        magic, version, flags, pg, start, mono = struct.unpack('<IHHIQQ', raw)
        if magic != 0x54574750 or version < 2 or start <= 0:
            raise ValueError(f'invalid trace header: {path}')
        headers.append(dict(file=path.name, start_wall_ns=str(start),
                            clock_offset_ns=str(mono), version=version))
    if not headers:
        raise ValueError(f'empty trace directory: {directory} (no trace headers)')
    return sorted(headers, key=lambda h: (int(h['start_wall_ns']), h['file']))


def serve(server, directory):
    """Test-only identity envelope. All data replies pass through verbatim.

    Read the actual child argv's directory, never an expected identity supplied
    by the caller. Missing/stranger servers cannot provide this envelope.
    Sequential JSON-line protocol is the production server's own protocol.
    """
    headers = trace_headers(directory)
    with subprocess.Popen([server, directory], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True) as child:
        try:
            for request in sys.stdin:
                child.stdin.write(request)
                child.stdin.flush()
                response = child.stdout.readline()
                if not response:
                    return child.wait() or 1
                if json.loads(request).get('cmd') == 'info':
                    data = json.loads(response)
                    data['_replay_trace_headers'] = headers
                    response = json.dumps(data) + '\n'
                sys.stdout.write(response)
                sys.stdout.flush()
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace-dir', required=True)
    parser.add_argument('--server', default=str(ROOT / 'pgwt-server'))
    parser.add_argument('--bridge', default=str(ROOT / 'web/pgwt'))
    parser.add_argument('--out-dir')
    parser.add_argument('--window-s', type=int, default=300)
    parser.add_argument('--timeout-s', type=int, default=30)
    parser.add_argument('--perturb-display', action='store_true',
                        help='RED: replace Overview DB Time cell after rendering')
    parser.add_argument('--serve', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.serve:
        return serve(args.server, args.trace_dir)
    if not args.out_dir:
        parser.error('--out-dir is required')
    out = Path(args.out_dir).resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('--out-dir must be outside the repository')
    if out.exists() and any(out.iterdir()):
        parser.error('--out-dir must be empty (preserve earlier evidence)')
    out.mkdir(parents=True, exist_ok=True)
    summary = dict(ok=False, mode='replay', tabs=[],
                   limitation='Presentation agreement cannot detect shared accounting errors.')
    try:
        if args.window_s <= 0 or args.timeout_s <= 0:
            raise ValueError('window and timeout must be positive')
        expected = trace_headers(args.trace_dir)
        import ui_live_smoke as smoke
        import ui_live_smoke_lib as lib
        from replay_ui_compare import ReplayProbe
        # Only discover the fixed window here. Comparison and coverage NEVER
        # query this second process: they use the UI's consumed WS responses.
        bootstrap = subprocess.run([args.server, args.trace_dir],
            input='{"id":1,"cmd":"info"}\n', text=True, capture_output=True,
            timeout=args.timeout_s, check=True)
        info = json.loads(bootstrap.stdout)
        to_ns = int(info['to_ns'])
        from_ns = max(int(info['from_ns']), to_ns - args.window_s * 10**9)
        if from_ns >= to_ns or not info.get('num_events'):
            raise ValueError('trace has no replayable events')
        summary.update(trace_headers=expected, from_ns=str(from_ns), to_ns=str(to_ns))
        with tempfile.TemporaryDirectory(prefix='pgwt-replay-') as tmp:
            launcher = Path(tmp) / 'server'
            command = [sys.executable, str(Path(__file__).resolve()), '--serve',
                       '--server', str(Path(args.server).resolve()), '--trace-dir']
            launcher.write_text('#!/bin/sh\nexec ' + shlex.join(command) + ' "$@"\n')
            launcher.chmod(0o700)
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            with (out / 'bridge.log').open('w') as log:
                bridge = subprocess.Popen([args.bridge, '--port', str(port),
                    '--server-path', str(launcher), '--trace-dir', str(Path(args.trace_dir).resolve()),
                    'root@127.0.0.1'], stdout=log, stderr=log)
                try:
                    base = f'http://127.0.0.1:{port}/'
                    deadline = time.monotonic() + args.timeout_s
                    while True:
                        if bridge.poll() is not None:
                            raise RuntimeError('bridge exited; see bridge.log')
                        try:
                            with urlopen(base + 'session', timeout=1):
                                break
                        except OSError:
                            if time.monotonic() >= deadline:
                                raise RuntimeError('bridge startup timeout')
                            time.sleep(.1)
                    url = base + f'#tab=overview&from={from_ns}&to={to_ns}'
                    smoke._require_playwright()
                    with smoke.sync_playwright() as pw:
                        browser = pw.chromium.launch(headless=True, args=['--no-sandbox'])
                        try:
                            for tab in lib.TABS:
                                probe = ReplayProbe(tab, expected, from_ns, to_ns, out,
                                                    perturb=args.perturb_display and tab == 'overview')
                                result = smoke.run_tab(browser, tab, url, str(out), ticks=0,
                                    first_data_timeout=args.timeout_s,
                                    blink_threshold=lib.BLINK_THRESHOLD,
                                    viewport={'width': 1710, 'height': 981},
                                    device_scale_factor=2, replay_probe=probe)
                                summary['tabs'].append(result)
                                detail = result.get('comparison') or result.get('error') or result
                                print(f"{'PASS' if result['ok'] else 'FAIL'} [{tab}]: {detail}", flush=True)
                        finally:
                            browser.close()
                finally:
                    bridge.terminate()
                    try:
                        bridge.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        bridge.kill()
                        bridge.wait()
        summary['ok'] = (len(summary['tabs']) == len(lib.TABS) and
                         all(r['ok'] for r in summary['tabs']))
    except Exception as exc:
        summary['error'] = str(exc)
        print(f'FAIL replay: {exc}', file=sys.stderr)
    finally:
        (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return 0 if summary['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
