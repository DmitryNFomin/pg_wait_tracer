#!/usr/bin/env python3
"""Reproduce the event investigation offline, reusing measure_wait_cpu unchanged.

Usage: python3 tests/results/pgsleep-outlier/investigate.py TRACE_DIR [OUTPUT_DIR]
Build pgwt-server first with ../perclass-wait-cpu/build_server.sh.
"""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
from measure_wait_cpu import Measurement, replay

TRACE = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).parent
OUT.mkdir(parents=True, exist_ok=True)


def save(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2) + '\n')


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


# Decode raw markers that execution_detail deliberately omits. This is not a
# second measurement implementation: all CPU reports below use Measurement.
binary = OUT / 'raw-neighbourhood'
compile_args = ['cc', '-O2', '-Wall', '-Wextra', '-DPGWT_SERVER', '-include',
                str(ROOT / 'demos/replay-compat.h'), '-I' + str(ROOT / 'src')]
if sys.platform == 'darwin':
    prefix = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    compile_args += ['-I' + prefix + '/include', '-L' + prefix + '/lib']
compile_args += [str(Path(__file__).with_name('raw_neighbourhood.c')),
                 str(ROOT / 'src/event_reader.c'), str(ROOT / 'src/event_writer.c'),
                 '-llz4', '-lz', '-lm', '-o', str(binary)]
with (OUT / 'raw-build.log').open('w') as log:
    subprocess.run(compile_args, stdout=log, stderr=log, check=True)


def raw(name, pid, start, end):
    path = OUT / (name + '-raw.jsonl')
    with path.open('w') as output, (OUT / 'raw-reader.log').open('a') as log:
        subprocess.run([str(binary), str(TRACE / 'current.trace'), str(pid),
                        str(start), str(end)], stdout=output, stderr=log, check=True)
    return records(path)


(OUT / 'raw-reader.log').write_text('')
all_waits = raw('locks-pgsleep', 0, 0, 2**64 - 1)
pg_rows = raw('pgsleep', 1171002, 1790897290000000000, 1790897330000000000)
outlier = next(r for r in pg_rows if r['cpu_ns'] == '923834')
txn = [r for r in all_waits if r['old_event'] == 50331653]
ties = [r for r in txn if r['cpu_ns'] == r['duration_ns']]
targets = {'pgsleep': outlier, 'transactionid-cpu': max(txn, key=lambda r: int(r['cpu_ns']))}
targets.update({f'transactionid-ratio-{i+1}': r for i, r in enumerate(ties)})
extend = [r for r in all_waits if r['old_event'] == 50331649]
targets.update({f'extend-{i+1}': r for i, r in enumerate(extend)})
save('targets.json', targets)

protocol = io.StringIO()
with replay(ROOT / 'pgwt-server', TRACE, OUT / 'measurement.server.log') as srv:
    info = srv.query('info', timeout=60)
    query = lambda cmd, **kw: srv.query(cmd, timeout=60, **kw)
    m = Measurement(query, 1790897300000000000, 1790897325000000000,
                    threshold=824430, evidence=protocol)
    result = m.run()
    result['server_info'] = info
    result['trace_dir'] = str(TRACE)
    with (TRACE / 'current.trace').open('rb') as source:
        result['trace_sha256'] = {'current.trace': hashlib.file_digest(source, 'sha256').hexdigest()}
    save('window-measurement.json', result)
    save('pgsleep-details.json', [json.loads(line) for line in protocol.getvalue().splitlines()
         if json.loads(line)['cmd'] == 'execution_detail'
         and json.loads(line)['args']['pid'] == 1171002])
    matched = {}
    for name, event in targets.items():
        start, end, pid = int(event['start_ns']), int(event['end_ns']), event['pid']
        if name != 'pgsleep':
            raw(name, pid, start - 1_000_000, end + 1_000_000)
        evidence = io.StringIO()
        def pid_query(cmd, **kw):
            if cmd == 'executions':
                kw['filters'] = {'pid': pid}
            return query(cmd, **kw)
        focus = Measurement(pid_query, start - 1_000_000, end + 1_000_000,
                            threshold=824430, evidence=evidence)
        focus.discover(focus.start, focus.end)
        for row in focus.rows.values():
            if row['n_events']:
                focus.detail(row)
        key = (pid, event['old_event'], start, int(event['duration_ns']))
        assert key in focus.events, (name, 'raw record missing from original tool')
        observed = focus.events[key]
        assert observed['cpu_ns'] == event['cpu_ns'], (name, observed, event)
        matched[name] = observed
        save(name + '-protocol.json', [json.loads(line) for line in evidence.getvalue().splitlines()])
    save('verified-events.json', matched)

metadata = {name: records(TRACE / name) for name in ('query_texts.jsonl', 'backends.jsonl')}
qids = {r['query_id'] for r in targets.values()}
pids = {r['pid'] for r in targets.values()}
save('backend-context.json', {
    'queries': [r for r in metadata['query_texts.jsonl'] if r['q'] in qids],
    'backends': [r for r in metadata['backends.jsonl'] if r['pid'] in pids]})
sleep = [r for r in pg_rows if r['old_event'] == 150994946]
i = sleep.index(outlier)
previous, following = sleep[i-1], sleep[i+1]
save('comparison.json', {
    'outlier': outlier, 'previous': previous, 'following': following,
    'cpu_duration_percent': 100 * int(outlier['cpu_ns']) / int(outlier['duration_ns']),
    'extra_wall_vs_following_ns': int(outlier['duration_ns']) - int(following['duration_ns']),
    'extra_cpu_vs_following_ns': int(outlier['cpu_ns']) - int(following['cpu_ns']),
    'remaining_extra_offcpu_ns': (int(outlier['duration_ns']) - int(outlier['cpu_ns']))
                              - (int(following['duration_ns']) - int(following['cpu_ns']))})
assert result['threshold_ns'] == 824430
assert result['classes']['Timeout:PgSleep']['above_threshold'] == 1
assert result['classes']['Timeout:PgSleep']['cpu_ns_max'] == 923834
assert result['status'] == 'REFUSED'  # Keep coverage refusal; never relabel clean.
binary.unlink()
print(f'Verified {len(matched)} targeted records against unchanged Measurement; '
      'window REFUSED, one PgSleep exceedance remains.')
