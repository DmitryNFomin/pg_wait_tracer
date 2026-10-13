"""Check measurement discipline and retain raw values beside the summaries."""
from collections import defaultdict
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else 'tests/results/matrix-regression/audit')
rows = [json.loads(line) for line in (root / 'measure.jsonl').read_text().splitlines()]
groups = defaultdict(list)
canaries = []
for row in rows:
    request = row['request']
    if request['cmd'] == 'time_model':
        assert request['to'] - request['from'] == 5_000_000_000, request
        if not row['excluded_cold']:
            canaries.append(row['ms'])
    elif request['cmd'] != 'info':
        assert request['to'] - request['from'] == 900_000_000_000, request
        if not row['excluded_cold']:
            assert row.get('curcache', {}).get('enabled') == 1, row
            groups[(row['arm'], row['rep'], request['cmd'])].append(row)
summary = {'endpoint': [], 'canary_5s_warm_ms': canaries, 'browser': [],
           'browser_cache': [], 'sequence': []}
for (arm, rep, command), samples in groups.items():
    decoded = [row['curcache']['decoded'] for row in samples]
    served = [row['curcache']['served'] for row in samples]
    assert len(set(decoded)) == 1, ('warm calls decoded additional blocks', arm, rep, command, decoded)
    assert all(b > a for a, b in zip(served, served[1:])), (arm, rep, command, served)
    if command == 'transitions':
        assert len(samples) == 6
    times = [row['ms'] for row in samples]
    summary['endpoint'].append(dict(arm=arm, rep=rep, command=command,
        warm_ms=times, median_ms=round(statistics.median(times), 3),
        decoded=decoded, served=served,
        total=samples[-1]['total'], hash=samples[-1]['hash'],
        phases=[row.get('phase') for row in samples]))
for rep in range(2):
    hashes = {next(row['hash'] for row in summary['endpoint']
                   if row['arm'] == arm and row['rep'] == rep and row['command'] == 'transitions')
              for arm in ['final', 'oldsort', 'untuned']}
    assert len(hashes) == 1, ('sort/allocator changed the response', rep)
browser = [json.loads(line) for line in (root / 'browser.jsonl').read_text().splitlines()]
paint_groups = defaultdict(list)
for row in browser:
    if not row['excluded_cold']:
        paint_groups[(row['arm'], row['rep'], row['tab'])].append(row)
    for item in row['wire']:
        assert item['request']['cmd'] != 'time_model'
for (arm, rep, tab), samples in paint_groups.items():
    assert len(samples) == 3, (arm, rep, tab, len(samples))
    times = [row['ttfp_ms'] for row in samples]
    summary['browser'].append(dict(arm=arm, rep=rep, tab=tab,
        warm_ttfp_ms=times, median_ms=statistics.median(times)))
assert len(paint_groups) == 3 * 2 * 6
browser_decode_patterns = []
for log in sorted(root.glob('browser-server-*.stderr')):
    lines = log.read_text().splitlines()
    assert sum(line.startswith('MXSERVER pid=') for line in lines) == 1
    reports = [dict((k, int(v)) for k, v in (part.split('=') for part in
                line.split('curcache ', 1)[1].split()))
               for line in lines if 'curcache enabled=' in line]
    assert len(reports) > 20 and all(row['enabled'] == 1 for row in reports)
    # Each Waterfall detail also loads an earlier planning-marker context.
    # Its 712 older blocks lie before the retained 900s cache suffix and are
    # decoded each cycle by design. Compare that actual cache pattern across
    # every arm; require no decoding on any ordinary 900s request or Matrix.
    assert len(reports) == 33 and all(row['resets'] == 0 for row in reports), log
    deltas = [b['decoded'] - a['decoded'] for a, b in zip(reports, reports[1:])]
    assert [i for i, delta in enumerate(deltas, 1) if delta] == [6, 14, 22, 30], (log, deltas)
    browser_decode_patterns.append([row['decoded'] for row in reports])
    summary['browser_cache'].append(dict(log=log.name, reports=reports,
        detail_context_decode_delta=[delta for delta in deltas if delta]))
assert all(pattern == browser_decode_patterns[0] for pattern in browser_decode_patterns)
sequence = [json.loads(line) for line in (root / 'sequence.jsonl').read_text().splitlines()]
for row in sequence:
    assert row.get('curcache', {}).get('enabled') == 1, row
    if row['name'] != 'raw_warmup':
        summary['sequence'].append(row)
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print('Validated warm calls, explicit cache statistics, 5s canaries, stable responses, and complete browser paints.')
for row in summary['endpoint']:
    print(row['arm'], row['rep'], row['command'], row['warm_ms'], 'median', row['median_ms'])
for row in summary['browser']:
    print('paint', row['arm'], row['rep'], row['tab'], row['warm_ttfp_ms'])
