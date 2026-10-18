#!/usr/bin/env python3
"""Real Chromium comparison against 907e070 (requires Playwright + Chromium).

Run with --url pointing at a running UI, or --mock. Keeps videos, summaries,
and raw timing samples under --out-dir. Default: three paired 6-tick visits
per tab per mode; no cadence acceleration. Run on an otherwise idle machine.
The fast registered tests and their mutations live in test_ui_live_smoke_footage.py.

A two-sided paired sign permutation test detects timing changes (alpha .05)
for capture cost and first-sample latency separately on EVERY tab. Raw means,
deltas and p-values are retained. Non-rejection is not proof of equivalence;
the separate exact protocol differential pins the default path's operations.
"""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
from unittest.mock import patch

import numpy as np
import ui_live_smoke as smoke
from test_ui_live_smoke_footage import baseline


def timing_report(before, after):
    a, b = np.asarray(before), np.asarray(after)
    assert len(a) == len(b) and len(a) >= 6, 'missing timing samples'
    delta = b - a
    rng = np.random.default_rng(907070)
    null = (rng.choice([-1, 1], size=(20000, len(delta))) * delta).mean(axis=1)
    p = float((np.count_nonzero(np.abs(null) >= abs(delta.mean())) + 1) / (len(null) + 1))
    return {'baseline_mean_ms': float(a.mean()), 'candidate_mean_ms': float(b.mean()),
            'mean_delta_ms': float(delta.mean()), 'paired_permutation_p': p,
            'samples': len(a), 'ok': p >= .05}


def visit(module, browser, tab, url, out, footage):
    from playwright.sync_api import Page, ElementHandle
    shots, navigation, tick_targets, mount_waits = [], [], [], []
    screenshot = Page.screenshot
    wait_tick = module._wait_for_tick
    wait_mount = module._wait_for_mount_at_or_after

    def capture(page, *args, **kwargs):
        shots.append(kwargs)
        return screenshot(page, *args, **kwargs)

    def tick(page, target, timeout):
        tick_targets.append((target, timeout))
        return wait_tick(page, target, timeout)

    def mount(page, tab_id, timestamp, timeout, **kwargs):
        result = wait_mount(page, tab_id, timestamp, timeout, **kwargs)
        mount_waits.append({'tab': tab_id, 'timeout': timeout,
                            'min_seq': kwargs.get('min_seq'), 'seq': result['seq']})
        return result

    def record(name, original):
        def observed(obj, *args, **kwargs):
            navigation.append((name, args, kwargs))
            return original(obj, *args, **kwargs)
        return observed

    with ExitStack() as stack:
        stack.enter_context(patch.object(Page, 'screenshot', capture))
        stack.enter_context(patch.object(module, '_wait_for_tick', tick))
        stack.enter_context(patch.object(module, '_wait_for_mount_at_or_after', mount))
        for cls, name in ((Page, 'goto'), (Page, 'click'),
                          (Page, 'wait_for_selector'), (ElementHandle, 'click')):
            stack.enter_context(patch.object(cls, name, record(name, getattr(cls, name))))
        result = module.run_tab(
            browser, tab, url, str(out), 6, 60, smoke.lib.BLINK_THRESHOLD,
            viewport={'width': 1710, 'height': 981}, device_scale_factor=2,
            **({'footage_only': True} if footage else {}))
    evidence = {'result': result, 'screenshots': shots, 'navigation': navigation,
                'tick_targets': tick_targets, 'mount_waits': mount_waits}
    (out / f'{tab}-observations.json').write_text(json.dumps(evidence, indent=2) + '\n')
    assert 'error' not in result, f'{tab}: {result.get("error")}'
    assert result['ticks_observed'] == 6, f'{tab}: tick count changed'
    assert len(shots) == (0 if footage else (36 if tab == 'timeline' else 30)), \
        f'{tab}: screenshot count {len(shots)}'
    assert Path(result['artifacts']['video']).stat().st_size > 0, 'video absent'
    if footage:
        for key in ('no_blink', 'capture_budget', 'frame_dims', 'frame_spacing'):
            assert result[key]['ok'] is None and result[key]['detail'] == smoke.FOOTAGE_NOT_EVALUATED
        assert result['ok'] is False
    return evidence


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--url')
    source.add_argument('--mock', action='store_true')
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--repeats', type=int, default=3)
    args = ap.parse_args()
    if args.repeats < 1:
        ap.error('--repeats must be positive')
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=False)
    base = baseline()
    proc = None
    evidence = {tab: {'baseline': [], 'default': [], 'footage': []} for tab in smoke.lib.TABS}
    smoke._require_playwright()
    try:
        # Launch first: an unavailable browser must fail, never write a green
        # timing report or start a server it cannot use.
        with smoke.sync_playwright() as p:
            browser = p.chromium.launch()
            try:
                if args.mock:
                    proc = smoke.start_mock_server(smoke.MOCK_PORT)
                url = smoke.mock_app_url(smoke.MOCK_PORT) if args.mock else args.url
                for repeat in range(args.repeats):
                    for tab in smoke.lib.TABS:
                        # Alternate order to reduce systematic warm-up bias.
                        modes = ['baseline', 'default'] if repeat % 2 == 0 else ['default', 'baseline']
                        for mode in [*modes, 'footage']:
                            target = out / f'{repeat}-{mode}'
                            target.mkdir(exist_ok=True)
                            print(f'{repeat + 1}/{args.repeats} {tab} {mode}', flush=True)
                            ev = visit(base if mode == 'baseline' else smoke, browser, tab,
                                       url, target, mode == 'footage')
                            evidence[tab][mode].append(ev)
                        a, b, f = [evidence[tab][mode][-1] for mode in ('baseline', 'default', 'footage')]
                        for other in (b, f):
                            assert a['navigation'] == other['navigation'], f'{tab}: navigation changed'
                            assert a['tick_targets'] == other['tick_targets'], f'{tab}: tick cadence changed'
                            assert len(a['mount_waits']) == len(other['mount_waits']) == 7
                            # First mount is navigation, then six live ticks.
                            mounts = other['mount_waits']
                            for index, m in enumerate(mounts[1:], 1):
                                assert m['tab'] == tab and m['timeout'] == smoke.TICK_TIMEOUT_S
                                assert m['min_seq'] == (None if index == 1 else mounts[index - 1]['seq'])
            finally:
                browser.close()
    finally:
        if proc is not None:
            smoke.stop_mock_server(proc)
    report = {}
    for tab, modes in evidence.items():
        samples = {}
        for mode in ('baseline', 'default'):
            ticks = [t for e in modes[mode] for t in e['result']['blink_sweep']['ticks']]
            samples[mode] = {
                'capture_ms': [t['capture_ms_total_ms'] for t in ticks],
                'first_sample_ms': [t['achieved_offsets_ms'][0] for t in ticks]}
        report[tab] = {metric: timing_report(samples['baseline'][metric], samples['default'][metric])
                       for metric in ('capture_ms', 'first_sample_ms')}
    (out / 'timing.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    assert all(v['ok'] for tab in report.values() for v in tab.values()), 'timing distributions differ'
    print('GREEN: capture counts, navigation, mount waits, tick targets, and timing checks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
