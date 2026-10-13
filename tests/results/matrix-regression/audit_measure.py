"""Run only under the box flock. Exclude cold calls; retain cache counters."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path('/root/cx-matrix-regression')
OUT = ROOT / 'audit'
TRACE = '/tmp/pgwt_cx_rehearsal_tqWPfm'
OUT.mkdir(exist_ok=True)
ARMS = ['before329', 'after329', 'after340', 'pre', 'after365',
        'before_refusal', 'after_refusal', 'aggregate', 'final', 'oldsort', 'untuned']


def run_arm(arm, rep):
    env = dict(os.environ, PGWT_CURRENT_TRACE_CACHE_STATS='1',
               PGWT_TRANSITIONS_PROVENANCE='1', PGWT_ALLOC_TUNE_DEBUG='1')
    if arm == 'untuned':
        env['PGWT_ALLOC_TUNE'] = '0'
    binary = ROOT / ('final' if arm == 'untuned' else arm) / 'pgwt-server'
    log_path = OUT / f'{arm}-{rep}.stderr'
    responses = []
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(binary), TRACE], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=log, text=True, env=env)
        seq = 0

        def ask(cmd, **params):
            nonlocal seq
            seq += 1
            req = dict(id=seq, cmd=cmd, **params)
            log.write('MXREQUEST ' + json.dumps(req) + '\n')
            log.flush()
            started = time.perf_counter()
            proc.stdin.write(json.dumps(req) + '\n')
            proc.stdin.flush()
            raw = proc.stdout.readline()
            elapsed = (time.perf_counter() - started) * 1000
            data = json.loads(raw)
            assert data['id'] == seq and not data.get('error'), data
            normalized = {k: v for k, v in data.items()
                          if k not in ('id', 'merged_blocks', 'decoded_blocks')}
            row = dict(arm=arm, rep=rep, request=req, ms=round(elapsed, 3),
                       response_bytes=len(raw), total=data.get('total'),
                       merged_blocks=data.get('merged_blocks'),
                       decoded_blocks=data.get('decoded_blocks'),
                       hash=hashlib.sha256(json.dumps(normalized, sort_keys=True).encode()).hexdigest())
            responses.append(row)
            return data, row

        try:
            info, _ = ask('info')
            end = info['to_ns']
            params = {'from': end - 900_000_000_000, 'to': end,
                      'buckets': 200, 'filters': {}}
            # Starting with aggregate edges retains only a suffix in the raw
            # cache. Prime the complete raw window first, as the UI's earlier
            # raw tabs do, so all arms compare genuinely cached records.
            _, warmup = ask('variants', **params)
            warmup['excluded_cold'] = True
            warmup['raw_cache_warmup'] = True
            for i in range(7):
                _, row = ask('transitions', **params)
                row['excluded_cold'] = i == 0
                if i in (0, 6):
                    for j in range(3):
                        _, canary = ask('time_model', **{'from': end - 5_000_000_000, 'to': end})
                        canary['canary_5s'] = True
                        canary['excluded_cold'] = j == 0
            if arm in ('pre', 'final', 'aggregate'):
                for cmd, extra in [('top_queries', {}),
                                   ('executions', {'limit': 100, 'sort': 'duration_desc'}),
                                   ('exec_scatter', {'max_points': 2000}),
                                   ('concurrency', {'buckets': 300})]:
                    for i in range(5):
                        _, row = ask(cmd, **(params | extra))
                        row['excluded_cold'] = i == 0
        finally:
            proc.stdin.close()
            proc.wait(timeout=30)
            assert proc.returncode == 0
    # Markers bracket the actual C reports, rather than inferring cache use from latency.
    by_id = {row['request']['id']: row for row in responses}
    current = None
    for line in log_path.read_text().splitlines():
        if line.startswith('MXREQUEST '):
            current = json.loads(line[len('MXREQUEST '):])['id']
        elif 'curcache enabled=' in line:
            by_id[current]['curcache'] = dict((k, int(v)) for k, v in
                (item.split('=') for item in line.split('curcache ', 1)[1].split()))
        elif line.startswith('MXPHASE '):
            phase = dict((k, float(v)) for k, v in
                         (item.split('=') for item in line.split()[1:]))
            by_id[int(phase['id'])]['phase'] = phase
    for row in responses:
        print(json.dumps(row), flush=True)


for rep in range(2):
    for arm in ARMS if rep == 0 else reversed(ARMS):
        run_arm(arm, rep)
