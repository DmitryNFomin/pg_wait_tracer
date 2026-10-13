"""Warm repeated calls in each single process; exact wire equality across arms."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
root = Path(sys.argv[1])
trace = sys.argv[2]
arm = sys.argv[3]
out = root / 'evidence'
out.mkdir(exist_ok=True)
with (out / f'{arm}.stderr').open('w') as log:
    proc = subprocess.Popen([str(root / arm / 'pgwt-server'), trace],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
        env=dict(os.environ, PGWT_CURRENT_TRACE_CACHE_STATS='1'), text=True)
    seq = 0
    def ask(cmd, **params):
        global seq
        seq += 1
        req = dict(id=seq, cmd=cmd, **params)
        log.write('EXREQUEST '+json.dumps(req)+'\n'); log.flush()
        started = time.perf_counter()
        proc.stdin.write(json.dumps(req)+'\n'); proc.stdin.flush()
        raw = proc.stdout.readline()
        elapsed = (time.perf_counter()-started)*1000
        data = json.loads(raw)
        assert data['id'] == seq and not data.get('error') and not data.get('unavailable'), data
        return req, raw, data, elapsed
    try:
        _, _, info, _ = ask('info')
        end = info['to_ns']
        params = {'from': end-900_000_000_000, 'to': end,
                  'filters': {}, 'limit': 100, 'sort': 'duration_desc'}
        for i in range(7):
            req, raw, data, ms = ask('executions', **params)
            wire = out / f'{arm}-execution-{i}.json'
            wire.write_text(raw)
            if arm != 'before':
                assert wire.read_bytes() == (out / f'before-execution-{i}.json').read_bytes(), 'wire differs'
            print(json.dumps(dict(arm=arm, call=i, excluded_cold=i==0, request=req,
                ms=ms, sha256=hashlib.sha256(raw.encode()).hexdigest(), bytes=len(raw),
                rows=len(data['rows']), total_count=data['total_count'], truncated=data['truncated'])), flush=True)
        for i in range(4):
            req, raw, data, ms = ask('transitions', **{'from':end-900_000_000_000,'to':end,'filters':{}})
            print(json.dumps(dict(arm=arm, canary='transitions_900s', call=i,
                excluded_cold=i==0, request=req, ms=ms)), flush=True)
    finally:
        proc.stdin.close(); proc.wait(timeout=30)
        assert proc.returncode == 0
