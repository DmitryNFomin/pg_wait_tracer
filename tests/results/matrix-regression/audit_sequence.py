"""Reproduce the historical overlapping windows with explicit cache stats."""
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path('/root/cx-matrix-regression')
WINDOWS = [('transitions_first', 1791800513880240400, 1791801413880240400),
           ('transitions_repeat1', 1791800513880240400, 1791801413880240400),
           ('transitions_repeat2', 1791800513880240400, 1791801413880240400),
           ('matrix_later1', 1791800683210846700, 1791801583210846700),
           ('matrix_later2', 1791800683210846700, 1791801583210846700),
           ('matrix_later3', 1791800683210846700, 1791801583210846700)]
for rep in range(2):
    for arm in ['pre', 'aggregate', 'final'] if rep == 0 else ['final', 'aggregate', 'pre']:
        log_path = ROOT / 'audit' / f'sequence-{arm}-{rep}.stderr'
        rows = []
        with log_path.open('w') as log:
            proc = subprocess.Popen([str(ROOT / arm / 'pgwt-server'), '/tmp/pgwt_cx_rehearsal_tqWPfm'],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                    text=True, env=dict(os.environ, PGWT_CURRENT_TRACE_CACHE_STATS='1',
                                                       PGWT_TRANSITIONS_PROVENANCE='1'))
            for i, (name, start, end) in enumerate([('raw_warmup', *WINDOWS[0][1:])] + WINDOWS):
                req = dict(id=i+1, cmd='variants' if i == 0 else 'transitions',
                           **{'from': start, 'to': end, 'buckets': 200, 'filters': {}})
                log.write('MXREQUEST ' + json.dumps(req) + '\n'); log.flush()
                started = time.perf_counter()
                proc.stdin.write(json.dumps(req) + '\n'); proc.stdin.flush()
                data = json.loads(proc.stdout.readline())
                assert data['id'] == req['id'] and not data.get('error'), data
                rows.append(dict(arm=arm, rep=rep, name=name, request=req,
                                 ms=round((time.perf_counter()-started)*1000, 3),
                                 **{k: data.get(k) for k in ('merged_blocks', 'decoded_blocks', 'total')}))
            proc.stdin.close(); proc.wait(timeout=30)
            assert proc.returncode == 0
        current = None
        for line in log_path.read_text().splitlines():
            if line.startswith('MXREQUEST '):
                current = json.loads(line[len('MXREQUEST '):])['id'] - 1
            elif 'curcache enabled=' in line:
                rows[current]['curcache'] = dict((k, int(v)) for k, v in
                    (item.split('=') for item in line.split('curcache ', 1)[1].split()))
        for row in rows:
            print(json.dumps(row), flush=True)
