#!/usr/bin/env python3
"""Black-box board CLI tests using JSON fixtures and a recording gh stub."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
BOARD = HERE / 'board.sh'
COLS = 'item what group state who started est0 est size note'.split()
STUB = '''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
root = pathlib.Path(os.environ['BOARD_TEST_ROOT'])
with (root / 'calls.jsonl').open('a') as stream:
    stream.write(json.dumps(args) + '\\n')
assert args[:2] == ['issue', 'edit'], 'offline mode called gh for a read'
assert args[3] == '--body-file', args
(root / 'published.md').write_bytes(pathlib.Path(args[4]).read_bytes())
'''


class BoardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='board-selftest-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        (self.bin / 'gh').write_text(STUB)
        (self.bin / 'gh').chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        BOARD_TEST_ROOT=str(self.root))
        self.env.pop('PGWT_BOARD_ISSUE', None)
        self.rows = [self.row('1'), self.row('2+3'), self.row('5'), self.row('6'), self.row('7'),
                     self.row('checklist', group='next', est0='', est='')]
        self.issues = [self.issue(1), self.issue(2, 'CLOSED'), self.issue(3), self.issue(4),
                       self.issue(5, 'CLOSED'), self.issue(6), self.issue(7),
                       self.issue(8, 'CLOSED', closed='2026-11-09T10:00:00Z')]
        self.issues[4]['labels'] = []
        self.prs = [self.pr(101, 'agent/1-fix', autoMergeRequest={'enabledAt': '2026-11-10T10:00:00Z'}),
                    self.pr(102, 'feature/fix', closingIssuesReferences=[{'number': 6}],
                            isDraft=True, statusCheckRollup={'state': 'FAILURE'}, mergeStateStatus='BEHIND'),
                    self.pr(103, 'agent/7-fix')]
        self.write()

    def row(self, item, **changes):
        row = dict.fromkeys(COLS, '')
        row.update(item=item, what='Work ' + item, group='progress', state='implementing', who='codex',
                   started='2026-11-10T10:00:00Z', est0='2026-11-10T11:00:00Z', est='2026-11-10T11:00:00Z')
        row.update(changes)
        return row

    def issue(self, n, state='OPEN', closed='2026-11-10T11:30:00Z'):
        return dict(number=n, title='Issue ' + str(n), state=state,
                    closedAt=closed if state == 'CLOSED' else None, labels=[{'name': 'tag-blocker'}])

    def pr(self, n, branch, **changes):
        pr = dict(number=n, headRefName=branch, isDraft=False, autoMergeRequest=None,
                  statusCheckRollup={'state': 'SUCCESS'}, mergeStateStatus='CLEAN', closingIssuesReferences=[])
        pr.update(changes)
        return pr

    def write(self, crlf=False, body=None):
        block = '<!-- plan:begin -->\n```tsv\n' + '\t'.join(COLS) + '\n'
        block += '\n'.join('\t'.join(row[c] for c in COLS) for row in self.rows)
        block += '\n```\n<!-- plan:end -->'
        if crlf:
            block = block.replace('\n', '\r\n')
        self.block = block
        board = dict(number=850, createdAt='2026-11-10T09:00:00Z',
                     body=body if body is not None else 'old header\n\n' + block + '\nold footer\n')
        for name, data in [('board', board), ('issues', self.issues), ('prs', self.prs)]:
            (self.root / (name + '.json')).write_text(json.dumps(data))
        (self.root / 'now.txt').write_text('2026-11-10T12:00:00Z\n')

    def cli(self, *args, ok=True, env=None, refusal='invalid input must refuse'):
        result = subprocess.run([str(BOARD), '--offline', str(self.root), *args],
                                env=env or self.env, text=True, capture_output=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, refusal)
        return result.stdout if ok else result.stderr

    def line(self, text, item):
        return next(line for line in text.splitlines() if line.startswith('| ' + item + ' |'))

    def published(self):
        return (self.root / 'published.md').read_bytes().decode()

    def persist(self):
        board = json.loads((self.root / 'board.json').read_text())
        board['body'] = self.published()
        (self.root / 'board.json').write_text(json.dumps(board))

    def test_derived_state(self):
        out = self.cli('render')
        self.assertIn('| merging |', self.line(out, '#1'), 'armed PR must beat plan state')
        self.assertIn('in review; checks failing; behind master; draft', self.line(out, '#6'),
                      'PR checks, branch lag and draft must be visible')
        self.assertIn('| in review |', self.line(out, '#7'), 'branch alone associates PR')
        self.assertIn('| done |', self.line(out, '#5 [NOT tag-blocker: #5]'), 'closed issue beats implementing')
        self.assertIn('11-10 11:30', self.line(out, '#5 [NOT tag-blocker: #5]'), 'closed time is actual')

    def test_multi_issue(self):
        out = self.cli('render')
        self.assertIn('| implementing |', self.line(out, '#2 + #3'), 'multi-issue item remains open until ALL close')
        self.issues[2].update(state='CLOSED', closedAt='2026-11-10T11:45:00Z')
        self.write()
        out = self.cli('render')
        self.assertIn('| done |', self.line(out, '#2 + #3'), 'all closed moves item to Done')
        self.assertIn('11-10 11:45', self.line(out, '#2 + #3'), 'actual is latest close time')

    def test_unplanned(self):
        out = self.cli('render')
        self.assertIn('| #4 | Issue 4 |', out.split('## Not on the plan yet')[1], 'unplanned open tag-blocker must appear')
        self.assertIn('Open tag-blockers: 5.', out, 'open count comes from labels')
        self.assertIn('Closed since board started: 1.', out, 'closed count excludes older and unlabelled issues')

    def test_estimates(self):
        self.cli('set', 'checklist', 'est=2026-11-10T13:00:00Z')
        first = self.published()
        self.assertIn('2026-11-10T13:00:00Z\t2026-11-10T13:00:00Z', first, 'first est must fill est0')
        self.persist()
        self.cli('set', 'checklist', 'est=2026-11-10T14:00:00Z')
        self.assertIn('Estimates moved: 1.', self.published(), 'moving est must raise moved count')
        self.assertIn('2026-11-10T13:00:00Z\t2026-11-10T14:00:00Z', self.published(), 'moving est must preserve est0')
        self.persist()
        before = (self.root / 'calls.jsonl').read_bytes()
        error = self.cli('set', 'checklist', 'est0=2026-11-10T15:00:00Z', ok=False,
                         refusal='rewriting est0 must be refused')
        self.assertIn('est0 is write-once', error, 'rewriting est0 must be refused')
        self.assertEqual((self.root / 'calls.jsonl').read_bytes(), before, 'refusal must not publish')
        self.cli('set', 'checklist', 'est0=', ok=False)
        self.rows[0]['est0'] = ''
        self.write()
        self.assertIn('Estimates moved: 1.', self.cli('render'),
                      'a missing first estimate must not hide a difference')

    def test_late(self):
        out = self.cli('render')
        self.assertIn('LATE by 1.0h', self.line(out, '#1'), 'past estimate must show LATE by hours')
        self.assertEqual(self.line(out, '#1').count('LATE by 1.0h'), 2,
                         'both first and current past estimates must be marked')
        self.assertIn('Past estimate, not done: 4.', out, 'late count excludes done rows')
        self.assertNotIn('LATE', self.line(out, '#5 [NOT tag-blocker: #5]'), 'done rows are not late')

    def test_preservation(self):
        self.write(crlf=True)
        self.cli('publish', '--issue', '999')
        expected = self.block.encode()
        data = (self.root / 'published.md').read_bytes()
        start = data.index(b'<!-- plan:begin -->')
        end = data.index(b'<!-- plan:end -->') + len(b'<!-- plan:end -->')
        self.assertEqual(data[start:end], expected, 'publish must preserve plan block byte for byte')
        call = json.loads((self.root / 'calls.jsonl').read_text().splitlines()[0])
        self.assertEqual(call[:3], ['issue', 'edit', '999'], '--issue must select publication target')

    def test_groups_and_unlabelled(self):
        self.rows[3]['group'] = 'done'  # open #6 with a PR still goes in progress
        self.rows[4]['group'] = 'done'  # open #7 too
        self.rows[1]['group'] = 'done'  # partially closed without a PR
        self.write()
        out = self.cli('render')
        progress = out.split('## In progress')[1].split('## Next')[0]
        self.assertIn('| #6 |', progress, 'PR moves planned-done item into progress')
        self.assertIn('| #2 + #3 |', progress, 'open issue cannot disappear into Done')
        self.assertIn('NOT tag-blocker: #5', out, 'unlabelled planned issue must be flagged')
        self.assertEqual([out.index('## ' + title) for title in
                          ['In progress', 'Next', 'Need a reproduction first', 'Done']],
                         sorted(out.index('## ' + title) for title in
                                ['In progress', 'Next', 'Need a reproduction first', 'Done']))

    def test_bootstrap_and_options(self):
        self.write(body='Legacy hand-written board')
        self.assertIn('No plan block stored', self.cli('render'), 'missing plan must be visible')
        self.cli('publish', ok=False)
        env = dict(self.env, PGWT_BOARD_ISSUE='998')
        self.cli('set', 'new', 'what=New task', 'est=2026-11-10T15:00:00Z', env=env)
        self.assertIn('| new | New task | not started |', self.published())
        call = json.loads((self.root / 'calls.jsonl').read_text().splitlines()[0])
        self.assertEqual(call[2], '998', 'environment override must select publication target')
        self.assertIn('<!-- plan:begin -->', self.published())

    def test_invalid_input(self):
        self.cli('set', '1', 'group=secret', ok=False)
        self.cli('set', '1', 'started=2026-11-10T12:00:00', ok=False)
        self.cli('set', '1', 'note=bad\nrow', ok=False)
        self.cli('set', '1', 'note=```', ok=False)
        self.write(body='<!-- plan:begin -->broken')
        self.assertIn('plan markers', self.cli('render', ok=False))
        self.write()
        (self.root / 'issues.json').unlink()
        self.cli('render', ok=False)
        self.assertFalse((self.root / 'calls.jsonl').exists(), 'invalid data must not reach gh')

    def test_escaping(self):
        self.rows[0].update(what='pipe | <tag>', note='note & more')
        self.write()
        self.assertIn('pipe \\| &lt;tag&gt; — note &amp; more', self.cli('render'), 'Markdown table cells must escape pipes and HTML')

    def test_live_query_contract(self):
        # Exercise the real query/normalization path against paginated gh JSON,
        # including overflow closing refs and an unlabelled planned issue.
        (self.bin / 'gh').write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['BOARD_TEST_ROOT'])
args = sys.argv[1:]
with (root / 'calls.jsonl').open('a') as stream:
    stream.write(json.dumps(args) + '\\n')
if args[:2] == ['repo', 'view']:
    print('owner/repo')
elif args[:2] == ['issue', 'view']:
    if args[2] == '850':
        print((root / 'board.json').read_text())
    else:
        assert args[2] == '5', args
        print(json.dumps(next(i for i in json.loads((root / 'issues.json').read_text()) if i['number'] == 5)))
else:
    assert args[:2] == ['api', 'graphql'], args
    assert '--paginate' in args and '--slurp' in args, 'must fetch every page'
    query = next(a for a in args if a.startswith('query='))
    if 'pullRequest(number:' in query:
        connection = 'closingIssuesReferences'
        batches = [[], [{'number': 6}]]
        def wrap(nodes):
            return {'pullRequest': {connection: {'nodes': nodes}}}
    elif 'pullRequests(first:' in query:
        assert 'commits(last:1)' in query, 'rollup is on the head commit'
        connection = 'pullRequests'
        prs = json.loads((root / 'prs.json').read_text())
        for pr in prs:
            pr['commits'] = {'nodes': [{'commit': {'statusCheckRollup': pr.pop('statusCheckRollup')}}]}
            refs = pr.pop('closingIssuesReferences')
            pr['closingIssuesReferences'] = {'nodes': [], 'pageInfo': {'hasNextPage': pr['number'] == 102}}
        batches = [prs[:1], prs[1:]]
        def wrap(nodes):
            return {connection: {'nodes': nodes}}
    else:
        assert 'labels:["tag-blocker"]' in query, query
        connection = 'issues'
        issues = [i for i in json.loads((root / 'issues.json').read_text()) if i['labels']]
        batches = [issues[:2], issues[2:]]
        def wrap(nodes):
            return {connection: {'nodes': nodes}}
    print(json.dumps([{'data': {'repository': wrap(nodes)}} for nodes in batches]))
''')
        result = subprocess.run([str(BOARD), 'render'], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, 'live query contract must render: ' + result.stderr)
        self.assertIn('Open tag-blockers: 5.', result.stdout, 'all issue pages must be included')
        self.assertIn('| merging |', self.line(result.stdout, '#1'), 'normalize GraphQL head commit rollup')
        self.assertIn('checks failing; behind master; draft', self.line(result.stdout, '#6'),
                      'overflow closing references must associate the PR')
        self.assertIn('NOT tag-blocker: #5', result.stdout, 'missing planned issue must be fetched')


def controls():
    # Each fault runs the SAME black-box assertion used for the real code.
    # Mutate and restore a private copy, never the working tree or real issue.
    mutations = [
        ('derived_state', "state = 'merging' if any(p['autoMergeRequest'] is not None for p in linked) else 'in review'",
         "state = row['state']", 'armed PR must beat plan state'),
        ('multi_issue', "complete = bool(ids) and all(issues[n]['state'].upper() == 'CLOSED' for n in ids)\n    actual = max((issues[n]['closedAt'] for n in ids), key=utc) if complete else ''",
         "complete = bool(ids) and any(issues[n]['state'].upper() == 'CLOSED' for n in ids)\n    actual = max((issues[n]['closedAt'] for n in ids if issues[n]['closedAt']), key=utc) if complete else ''",
         'multi-issue item remains open until ALL close'),
        ('unplanned', "if issue['number'] not in planned:", 'if False:', 'unplanned open tag-blocker must appear'),
        ('estimates', "if row['est0'] and 'est0' in changes and changes['est0'] != row['est0']:",
         'if False:', 'rewriting est0 must be refused'),
        ('estimates', "moved = sum(r['est'] != r['est0'] for r in rows)",
         'moved = 0', 'moving est must raise moved count'),
        ('late', "text += f' (LATE by {hours:.1f}h)'", "text += ''", 'past estimate must show LATE by hours'),
        ('preservation', "out += ['', block]", "out += ['', block.replace('\\r\\n', '\\n')]",
         'publish must preserve plan block byte for byte'),
        ('groups_and_unlabelled', "item += ' [NOT tag-blocker: #' + ', #'.join(bad) + ']'",
         "item += ''", 'unlabelled planned issue must be flagged'),
        ('bootstrap_and_options', "if not block:\n        out +=", "if False:\n        out +=", 'missing plan must be visible'),
        ('invalid_input', "if row['group'] not in dict(GROUPS):", 'if False:', 'invalid input must refuse'),
        ('escaping', ".replace('|', '\\\\|')", ".replace('|', '|')", 'Markdown table cells must escape pipes and HTML'),
        ('live_query_contract', 'commits(last:1)', 'commits(first:1)', 'live query contract must render'),
    ]
    with tempfile.TemporaryDirectory(prefix='board-negative-') as directory:
        root = Path(directory)
        shutil.copy2(BOARD, root / 'board.sh')
        source = (HERE / 'board.py').read_text()
        for name, old, new, assertion in mutations:
            if source.count(old) != 1:
                raise AssertionError('mutation anchor must be unique: ' + old)
            (root / 'board.py').write_text(source.replace(old, new))
            run = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--board', str(root / 'board.sh'),
                                  '--case', 'test_' + name], capture_output=True, text=True)
            output = run.stdout + run.stderr
            (root / 'board.py').write_text(source)
            if run.returncode != 1 or assertion not in output or 'AssertionError' not in output:
                raise AssertionError('negative control failed to detect fault: ' + name + '\n' + output)
            lines = [line for line in output.splitlines() if 'AssertionError' in line or line.startswith('FAIL:')]
            print('Expected failing assertion: ' + assertion, flush=True)
            print('NEGATIVE CONTROL ' + name + ': ' + '\n'.join(line[:320] for line in lines), flush=True)
            print('RESTORED isolated board.py: PASS', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--case')
    parser.add_argument('--board', type=Path, default=BOARD)
    args = parser.parse_args()
    BOARD = args.board.resolve()
    if args.negative_controls:
        controls()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BoardTests)
    if args.case:
        suite = unittest.TestSuite([BoardTests(args.case)])
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
