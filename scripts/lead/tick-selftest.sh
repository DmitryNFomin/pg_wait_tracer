#!/usr/bin/env bash
# Offline black-box checks, then one deliberately broken copy per required case.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 -B - "$here" <<'PY'
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

SOURCE = Path(sys.argv[1])
FAKE = '''#!/usr/bin/env python3
import json, os, pathlib, sys
f = json.loads(pathlib.Path(os.environ['TICK_FIXTURE']).read_text())
args = sys.argv[1:]
tool = pathlib.Path(sys.argv[0]).name
with open(os.environ['TICK_LOG'], 'a') as log:
    log.write(json.dumps([tool] + args) + '\\n')
if f.get('failure') == tool:
    print('injected ' + tool + ' failure', file=sys.stderr)
    sys.exit(3)
if tool == 'gh' and os.environ.get('GH_REPO'):
    print('caller GH_REPO redirected the tick', file=sys.stderr)
    sys.exit(4)
if tool == 'git':
    print(os.environ['TICK_REPO'])
elif tool == 'ps':
    for proc in f.get('processes', []):
        print(str(proc['pid']) + ' ' + proc.get('stat', 'S') + ' ' + proc['args'])
elif tool == 'lsof':
    pid = int(args[args.index('-p') + 1])
    proc = next(p for p in f['processes'] if p['pid'] == pid)
    print('p' + str(pid) + '\\nn' + proc['cwd'])
elif args[:2] == ['repo', 'view']:
    print(json.dumps({'nameWithOwner': 'owner/repo'}))
elif args[:2] == ['api', 'graphql']:
    query = next(x for x in args if x.startswith('query='))
    if 'pullRequests(' in query:
        nodes = [dict(pr) for pr in f['prs']]
        if 'baseRefName' not in query:
            for pr in nodes:
                pr.pop('baseRefName', None)
        value = {'pullRequests': {'nodes': nodes}}
    else:
        number = next(x.split('=')[1] for x in args if x.startswith('number='))
        value = {'pullRequest': {'commits': {'nodes': [{'commit': {
            'statusCheckRollup': {'contexts': {'nodes': f.get('checks', {}).get(number, [])}}
        }}]}}}
    print(json.dumps(f.get('graphql_pages', [{'data': {'repository': value}}])))
elif args[:2] == ['pr', 'update-branch']:
    print('updated')
elif args[:2] == ['pr', 'view']:
    print(json.dumps(f.get('follow_pr', {'state': 'OPEN', 'mergeStateStatus': 'CLEAN'})))
elif args[0] == 'api' and '/compare/' in args[1]:
    print(json.dumps({'files': f.get('files', [{'filename': 'docs/readme.md'}])}))
elif args[0] == 'api' and '/actions/runs?' in args[1]:
    print(json.dumps([{'workflow_runs': f.get('runs', [])}]))
else:
    print('unexpected fake call: ' + repr(args), file=sys.stderr)
    sys.exit(4)
'''


def pr(number=1, state='CLEAN', age=0, created='2026-01-01T00:00:00Z', armed=True, base='master'):
    return dict(number=number, createdAt=created,
                updatedAt=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() - age)),
                headRefOid=f'head{number}', baseRefName=base, mergeStateStatus=state, state='OPEN',
                autoMergeRequest={'enabledAt': created} if armed else None)


class Fixture:
    def __init__(self, root, mutant=None):
        self.root = root
        self.repo = root / 'repo'
        self.here = self.repo / 'scripts/lead'
        self.here.mkdir(parents=True)
        shutil.copy2(SOURCE / 'tick.sh', self.here)
        source = (SOURCE / 'tick.py').read_text()
        if mutant:
            old, new = mutant
            assert source.count(old) == 1, f'mutation target must be unique: {old}'
            source = source.replace(old, new)
        compile(source, str(self.here / 'tick.py'), 'exec')
        (self.here / 'tick.py').write_text(source)
        self.bin = root / 'bin'
        self.bin.mkdir()
        for name in ('gh', 'git', 'ps', 'lsof'):
            path = self.bin / name
            path.write_text(FAKE)
            path.chmod(0o755)
        self.log = root / 'calls.jsonl'
        self.file = root / 'fixture.json'
        self.base = root / 'state'
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        PGWT_LEAD_STATE_DIR=str(self.base), TICK_FIXTURE=str(self.file),
                        TICK_LOG=str(self.log), TICK_REPO=str(self.repo), GH_REPO='wrong/repo')
        self.data = {'prs': []}

    def call(self, *args, timeout=5):
        self.file.write_text(json.dumps(self.data))
        try:
            result = subprocess.run(['bash', str(self.here / 'tick.sh'), *args],
                                    cwd=self.root, env=self.env, text=True,
                                    capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise AssertionError('watch must exit on a new item') from None
        assert result.stderr == '', f'unexpected stderr: {result.stderr}'
        return result.returncode, result.stdout

    def calls(self):
        return [json.loads(row) for row in self.log.read_text().splitlines()]

    def updates(self):
        return [row[3] for row in self.calls() if row[:3] == ['gh', 'pr', 'update-branch']]

    def state(self):
        return self.base / 'tick-state/state.json'


def needs(result, fragment, assertion):
    rc, out = result
    assert rc == 10 and fragment in out, assertion


def safe(f):
    f.data['prs'] = [pr(2, 'BEHIND', created='2026-02-01T00:00:00Z'), pr(1, 'BEHIND')]
    rc, out = f.call('once')
    assert f.updates() == ['1'] and 'ACTED update-branch 1\n' in out, 'safe oldest PR must be updated exactly once'
    assert 'NEEDS-LEAD pr-1-' not in out, 'updated PR must not raise NEEDS-LEAD'
    assert rc == 10 and 'NEEDS-LEAD pr-2-behind' in out, 'other BEHIND PR must remain visible'


def unsafe(f):
    f.data.update(prs=[pr(state='BEHIND')], files=[{'filename': 'src/reader.c'}])
    result = f.call('once')
    needs(result, 'master delta touches src/reader.c; box-check before update-branch', 'src delta must require judgement')
    assert f.updates() == [], 'src delta must never update-branch'


def stacked(f):
    # The master delta looks safe, but update-branch would merge agent/x.
    f.data['prs'] = [pr(state='BEHIND', base='agent/x')]
    result = f.call('once')
    needs(result, 'NEEDS-LEAD pr-1-behind behind; base agent/x is not master',
          'non-master base must require judgement even with a safe master delta')
    assert f.updates() == [], 'non-master base must never update-branch'


def graphql_page(f):
    for page in (None, False, 7, 'bad', []):
        f.data['graphql_pages'] = [page]
        needs(f.call('once'), 'NEEDS-LEAD tick-error gh graphql: expected an object page',
              'non-object GraphQL page must be an explicit tick-error')


def dirty(f):
    f.data['prs'] = [pr(state='DIRTY')]
    needs(f.call('once'), 'NEEDS-LEAD pr-1-dirty DIRTY', 'DIRTY must wake the lead')


def failed(f):
    f.data.update(prs=[pr()], checks={'1': [dict(name='required gate', conclusion='FAILURE', isRequired=True)]})
    needs(f.call('once'), 'failed required check: required gate', 'failed required check must wake the lead')


def stalled(f):
    f.data['prs'] = [pr(age=1900)]
    needs(f.call('once'), 'NEEDS-LEAD pr-1-stalled stalled', 'stalled head must wake the lead')


def follow(f):
    rc, ident = f.call('follow', 'add', 'merged:99', 'review next slice')
    assert rc == 0 and ident == 'follow-1\n', 'follow add must return its stable id'
    assert f.call('once') == (0, ''), 'unfired follow-up must wait'
    f.data['follow_pr'] = {'state': 'MERGED', 'mergeStateStatus': 'UNKNOWN'}
    needs(f.call('once'), 'NEEDS-LEAD follow-1 merged:99; review next slice', 'fired follow-up must wake the lead')
    f.call('ack')
    # Trigger no longer true: fired state must be durable, not recalculated away.
    f.data['follow_pr'] = {'state': 'OPEN', 'mergeStateStatus': 'CLEAN'}
    needs(f.call('once'), 'NEEDS-LEAD follow-1', 'fired follow-up must repeat even after ack')
    needs(f.call('once'), 'NEEDS-LEAD follow-1', 'follow-up must repeat until done')
    assert f.call('follow', 'done', 'follow-1') == (0, ''), 'follow done must succeed'
    assert f.call('once') == (0, ''), 'done follow-up must stop reporting'
    assert 'follow-1 done merged:99' in f.call('follow', 'list')[1], 'list must retain completed follow-ups'


def gh_error(f):
    f.data['failure'] = 'gh'
    needs(f.call('once'), 'NEEDS-LEAD tick-error gh repo view', 'gh failure must be an explicit tick-error')
    f.call('ack')
    needs(f.call('once'), 'NEEDS-LEAD tick-error', 'ack must never silence errors')


def quiet(f):
    assert f.call('once') == (0, ''), 'empty tick must be silent and successful'


def dedup(f):
    f.data['prs'] = [pr(state='DIRTY')]
    needs(f.call('once'), 'NEEDS-LEAD pr-1-dirty', 'unacked item must report')
    f.call('ack')
    assert f.call('once') == (0, ''), 'acked item must be silent before 60 minutes'
    state = json.loads(f.state().read_text())
    state['seen'] = {key: time.time() - 3590 for key in state['seen']}
    f.state().write_text(json.dumps(state))
    assert f.call('once') == (0, ''), 'acked item must remain silent just before 60 minutes'
    state['seen'] = {key: time.time() - 3601 for key in state['seen']}
    f.state().write_text(json.dumps(state))
    needs(f.call('once'), 'NEEDS-LEAD pr-1-dirty', 'acked item must report again after 60 minutes')


def watch(f):
    f.data['prs'] = [pr(state='DIRTY')]
    needs(f.call('watch', '--interval', '0.02', timeout=1.5), 'NEEDS-LEAD pr-1-dirty', 'watch must exit on a new item')


CASES = [
    ('safe BEHIND updates only oldest', safe,
     ("command('gh', 'pr', 'update-branch', str(n), '--repo', repo)", 'pass'),
     'safe oldest PR must be updated exactly once'),
    ('src delta refuses update', unsafe,
     ("path.startswith('src/') or path == 'tests/run_all.sh'", "False"),
     'src delta must require judgement'),
    ('non-master base refuses update', stacked,
     ("and pr['baseRefName'] != 'master'", 'and False'),
     'non-master base must require judgement even with a safe master delta'),
    ('non-object GraphQL page', graphql_page,
     ("raise RuntimeError('gh graphql: expected an object page')", 'continue'),
     'non-object GraphQL page must be an explicit tick-error'),
    ('armed DIRTY', dirty,
     ("if pr['mergeStateStatus'] == 'DIRTY':", 'if False:'), 'DIRTY must wake the lead'),
    ('failed required check', failed,
     ('if failed:', 'if False:'), 'failed required check must wake the lead'),
    ('stalled head', stalled,
     ("if not active and pr['state'] != 'MERGED':", 'if False:'), 'stalled head must wake the lead'),
    ('persistent fired follow-up', follow,
     ("if follow['fired']:", 'if False:'), 'fired follow-up must wake the lead'),
    ('gh failure', gh_error,
     ("items.append(line('tick-error', exc))", 'pass'), 'gh failure must be an explicit tick-error'),
    ('nothing to do', quiet,
     ('items, acted = [], []', "items, acted = [line('broken', 'spurious wake')], []"),
     'empty tick must be silent and successful'),
    ('ack and 60 minute reminder', dedup,
     ("or item not in data['seen']", "or True or item not in data['seen']"),
     'acked item must be silent before 60 minutes'),
    ('watch exits on new item', watch,
     ("if rc or args.command == 'once':", "if args.command == 'once':"),
     'watch must exit on a new item'),
]

with tempfile.TemporaryDirectory(prefix='pgwt-tick-selftest-') as temp:
    root = Path(temp)
    for index, (name, case, mutation, intended) in enumerate(CASES):
        case(Fixture(root / f'case-{index}'))
        try:
            case(Fixture(root / f'mutant-{index}', mutation))
        except AssertionError as exc:
            assert str(exc) == intended, f'{name}: mutant failed wrong assertion: {exc}'
        else:
            raise AssertionError(f'{name}: deliberately broken version survived')

    f = Fixture(root / 'extra')
    f.data.update(prs=[pr(state='BEHIND')], files=[{'filename': 'docs/moved.c', 'previous_filename': 'src/moved.c'}])
    needs(f.call('once'), 'box-check before update-branch', 'rename out of src must remain unsafe')
    assert f.updates() == []
    f.data['files'] = [{'filename': 'tests/run_all.sh'}]
    needs(f.call('once'), 'tests/run_all.sh', 'run_all delta must remain unsafe')
    assert f.updates() == []
    f.data['files'] = [{'filename': f'docs/{n}.md'} for n in range(300)]
    needs(f.call('once'), 'file list may be truncated', 'truncated delta must request judgement')
    assert f.updates() == []

    f = Fixture(root / 'check-types')
    f.data.update(prs=[pr()], checks={'1': [dict(context='legacy required', state='ERROR', isRequired=True)]})
    needs(f.call('once'), 'legacy required', 'required legacy status failure must wake')
    f.data['checks']['1'][0]['isRequired'] = False
    assert f.call('once') == (0, ''), 'optional failure must not wake'
    f.data.update(prs=[pr(age=1900)], runs=[{'status': 'queued'}], checks={})
    assert f.call('once') == (0, ''), 'queued run must prevent stalled report'
    f.data['runs'] = [{'status': 'in_progress'}]
    assert f.call('once') == (0, ''), 'in progress run must prevent stalled report'
    f.data['prs'] = [pr(state='DIRTY', armed=False)]
    assert f.call('once') == (0, ''), 'unarmed PR must not wake'

    f = Fixture(root / 'state-follow')
    assert f.call('follow', 'add', 'state:99:CLEAN', 'take next action') == (0, 'follow-1\n')
    needs(f.call('once'), 'state:99:CLEAN; take next action', 'state trigger must fire')
    f.call('follow', 'done', 'follow-1')
    f.data['failure'] = 'git'
    needs(f.call('once'), 'NEEDS-LEAD tick-error git', 'git failure must wake')

    f = Fixture(root / 'slots')
    f.base.mkdir()
    (f.base / 'ready.txt').write_text('implement next slice\nsecond item\n')
    needs(f.call('once'), 'NEEDS-LEAD slots free slots 3; ready: implement next slice', 'ready work and free slots must wake')
    # Use actual live PIDs/cwds, with ps commands supplied by the fixture. This
    # exercises /proc on Linux and the fake lsof on macOS without a test-only
    # production override. Clean up all spawned processes and directories.
    process_root = Path('/private/tmp/pgwt-codex')
    process_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tick-process-', dir=process_root) as work:
        children = [subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'], cwd=work)
                    for _ in range(3)]
        try:
            f.data['processes'] = [dict(pid=p.pid, cwd=work, args='codex exec task') for p in children]
            assert f.call('once') == (0, ''), 'three running implementers must consume all slots'
            f.data['processes'][0]['args'] = 'sh -c "codex exec task"'
            needs(f.call('once'), 'free slots 1;', 'shell command text must not count as codex process')
            f.data['processes'][0]['args'] = 'codex exec task'
            f.data['processes'][0]['stat'] = 'Z'
            needs(f.call('once'), 'free slots 1;', 'zombie must not count as running implementer')
        finally:
            for p in children:
                p.terminate()
                p.wait()

    # watch must stay asleep on an acked item, then exit when its reason changes.
    f = Fixture(root / 'watch-new')
    f.data['prs'] = [pr(state='DIRTY')]
    f.call('once')
    f.call('ack')
    f.file.write_text(json.dumps(f.data))
    proc = subprocess.Popen(['bash', str(f.here / 'tick.sh'), 'watch', '--interval', '0.02'],
                            env=f.env, cwd=f.root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(0.35)
        assert proc.poll() is None, 'watch must keep sleeping for an acked item'
        f.data['prs'] = [pr(age=1900)]
        pending = f.file.with_suffix('.tmp')
        pending.write_text(json.dumps(f.data))
        pending.replace(f.file)
        out, err = proc.communicate(timeout=5)
        assert proc.returncode == 10 and 'stalled' in out and err == '', 'watch must exit when a new item appears'
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

print('PASS safe-oldest, src-delta, non-master-base, graphql-page, DIRTY, failed-check, stalled, follow-repeat-until-done, gh-error, quiet, 60m-dedup, watch')
print('PASS negative controls: 12 compiled mutants reached their cases and failed the intended assertions')
print('PASS extras: run_all/src-rename/truncation; legacy/optional/active/unarmed checks; state-follow/git-error; slots; watch-acked/new')
print('tick-selftest: PASS (12 required cases with caught mutants; 5 extra groups)')
PY
