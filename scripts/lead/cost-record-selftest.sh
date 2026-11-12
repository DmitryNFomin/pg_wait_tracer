#!/usr/bin/env bash
# Offline CLI checks with a fake gh, including deliberately broken copies.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 -B - "$here" <<'PY'
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(sys.argv[1]) / 'cost-record.sh'
FAKE = '''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['COST_LOG'], 'a') as stream:
    stream.write(json.dumps(args) + '\\n')
if args == ['pr', 'view', '99', '--json', 'headRefName', '--jq', '.headRefName']:
    print(os.environ.get('COST_HEAD', 'agent/slice'))
elif args[:3] == ['pr', 'comment', '99'] and args[3:4] == ['--body-file'] and len(args) == 5:
    pathlib.Path(os.environ['COST_POST']).write_text(pathlib.Path(args[4]).read_text())
else:
    print('unexpected gh call: ' + repr(args), file=sys.stderr)
    sys.exit(4)
'''


class Fixture:
    def __init__(self, root, mutation=None):
        root.mkdir()
        self.root = root
        self.script = root / 'cost-record.sh'
        source = SOURCE.read_text()
        if mutation:
            old, new = mutation
            assert source.count(old) == 1, f'mutation target must be unique: {old}'
            source = source.replace(old, new)
        self.script.write_text(source)
        checked = subprocess.run(['bash', '-n', str(self.script)], capture_output=True)
        assert checked.returncode == 0, 'mutant must parse'
        gh = root / 'gh'
        gh.write_text(FAKE)
        gh.chmod(0o755)
        self.log, self.post = root / 'calls.jsonl', root / 'posted'
        self.env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                        PGWT_LEAD_STATE_DIR=str(root), COST_LOG=str(self.log), COST_POST=str(self.post))
        self.ledger = root / 'costs.tsv'
        self.rows()

    def rows(self, second='250'):
        self.ledger.write_text(
            '2026-11-12T19:00:00Z\tslice\t/brief\tsha\t100\t5\t0\tAUTO_COMMIT\n'
            '2026-11-12T19:01:00Z\tother\t/brief\tsha\t999\t6\t0\tAUTO_COMMIT\n'
            f'2026-11-12T19:02:00Z\tslice\t/brief\tsha\t{second}\t7\t124\tBUDGET_EXCEEDED\n')

    def call(self, *args):
        return subprocess.run(['bash', str(self.script), '99', *args], env=self.env,
                              cwd=self.root, capture_output=True, text=True)


def draft(f):
    result = f.call()
    assert result.returncode == 0 and result.stderr == '', 'draft must succeed'
    out = result.stdout
    assert 'Cost record' in out.splitlines()[0], 'first line must identify cost record'
    assert '- Codex round 1: 100 tokens; AUTO_COMMIT' in out, 'first branch round must appear'
    assert '- Codex round 2: 250 tokens; BUDGET_EXCEEDED' in out, 'second branch round must appear'
    assert '999' not in out and out.count('- Codex round ') == 2, 'draft must filter unrelated branch'
    assert all(text in out for text in ('Reviewer tokens: [FILL IN', 'Adviser tokens: [FILL IN',
                                        'Gate-box hours: [FILL IN')), 'lead placeholders must appear'


def total(f):
    assert 'Codex total: 350 tokens (2 rounds).' in f.call().stdout, 'total must sum branch rounds'


def unknown(f):
    f.rows('unknown')
    out = f.call().stdout
    assert '- Codex round 2: unknown tokens; BUDGET_EXCEEDED' in out, 'unknown round must appear'
    assert 'Codex total: unknown (known subtotal: 100 tokens; 1 rounds unknown).' in out, 'unknown must not become zero in total'


def post(f):
    result = f.call('--post')
    assert result.returncode == 0, 'post must use gh pr comment'
    assert f.post.read_text() == result.stdout, 'posted body file must equal printed draft'
    calls = [json.loads(row) for row in f.log.read_text().splitlines()]
    assert calls[-1][:4] == ['pr', 'comment', '99', '--body-file'], 'post must target PR with body file'
    assert not Path(calls[-1][4]).exists(), 'draft temp file must be cleaned'


def draft_only(f):
    assert f.call().returncode == 0
    assert not f.post.exists(), 'default draft must not post'


CASES = [
    ('branch rounds', draft, ("if len(row) > 1 and row[1] == branch", 'if len(row) > 1'),
     'second branch round must appear'),
    ('total', total, ("total = sum(int(row[4]) for row in rows if row[4] != 'unknown')", 'total = 0'),
     'total must sum branch rounds'),
    ('unknown', unknown, ('if unknown:', 'if False:'), 'unknown must not become zero in total'),
    ('post to PR', post, ('gh pr comment', 'gh issue comment'), 'post must use gh pr comment'),
    ('draft only', draft_only, ('post=0', 'post=1'), 'default draft must not post'),
]

with tempfile.TemporaryDirectory(prefix='pgwt-cost-record-selftest-') as temp:
    root = Path(temp)
    for index, (name, case, mutation, intended) in enumerate(CASES):
        case(Fixture(root / f'case-{index}'))
        try:
            case(Fixture(root / f'mutant-{index}', mutation))
        except AssertionError as exc:
            assert str(exc) == intended, f'{name}: wrong mutant assertion: {exc}'
        else:
            raise AssertionError(f'{name}: mutant survived')
    f = Fixture(root / 'unprefixed')
    f.env['COST_HEAD'] = 'slice'
    draft(f)
    f.env['COST_HEAD'] = 'agent/missing'
    result = f.call('--post')
    assert result.returncode != 0 and 'no cost rows' in result.stderr and not f.post.exists()
    f.env['COST_HEAD'] = 'slice'
    f.ledger.unlink()
    assert f.call('--post').returncode != 0 and not f.post.exists()

print('PASS draft branch rounds, total, unknown subtotal, reviewer/adviser/box placeholders, PR body-file post, draft-only, unprefixed branch, missing ledger/rows refusal')
print('PASS negative controls: 5 parsed mutants reached their cases and failed intended assertions')
print('cost-record-selftest: PASS')
PY
