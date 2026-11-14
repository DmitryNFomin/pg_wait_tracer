#!/usr/bin/env bash
# Enforce the pilot boundary; self-test seeds each forbidden kind outside the repo.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -B - "$repo" "$@" <<'PY'
import pathlib, re, shutil, subprocess, sys, tempfile
repo=pathlib.Path(sys.argv[1])
args=sys.argv[2:]
c_names=('pgwt_core_prepare','pgwt_core_wait_metrics','pgwt_evidence_replay',
         'pgwt_codec_decode','pgwt_core_evidence','pgwt_evidence_output')
js_kinds=(r'Number\s*\(',r'parseInt',r'parseFloat',r'Math\s*\.',r'reduce\s*\(')
js_files=('web/static/views/events-pilot.js','web/static/lib/builders/pilot-evidence.js')
def check(root):
    violations=[]
    for path in sorted((root/'src').glob('*.c')):
        for name in c_names:
            if re.search(r'\b'+name+r'\w*\b',path.read_text()): violations.append(f'{path.name}: {name}')
    for relative in js_files:
        for pattern in js_kinds:
            if re.search(pattern,(root/relative).read_text()): violations.append(f'{relative}: {pattern}')
    for violation in violations: print('BOUNDARY VIOLATION '+violation)
    return not violations
if args and args[0]=='--root':
    sys.exit(0 if check(pathlib.Path(args[1])) else 1)
assert check(repo), 'baseline boundary is red'
print('consumer boundary: PASS')
if args:
    assert args==['--self-test']
    with tempfile.TemporaryDirectory(prefix='pgwt-consumer-boundary-') as work:
        root=pathlib.Path(work)
        shutil.copytree(repo/'src',root/'src',ignore=shutil.ignore_patterns('core','bpf'))
        for relative in js_files:
            target=root/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/relative,target)
        seeds=[('src/seed.c',name+'();',name) for name in c_names]
        seeds += [(js_files[0],code,label) for code,label in
                  [('Number("1");','Number('),('parseInt("1");','parseInt'),('parseFloat("1");','parseFloat'),
                   ('Math.max(1,2);','Math.'),('[1].reduce(x=>x);','reduce(')]]
        for relative,code,label in seeds:
            path=root/relative
            original=path.read_text() if path.exists() else None
            path.write_text((original or '')+'\n'+code+'\n')
            result=subprocess.run(['bash',str(repo/'tests/test_consumer_boundary.sh'),'--root',str(root)],capture_output=True,text=True)
            assert result.returncode==1 and 'BOUNDARY VIOLATION' in result.stdout,(label,result)
            print('seed '+label+': RED (exit 1)')
            if original is None: path.unlink()
            else: path.write_text(original)
    print('consumer boundary seeded violations: 11/11 RED')
PY
