#!/usr/bin/env bash
# Real runners in temporary checkout copies: no edits to the invoking tree.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - <<'PY'
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path.cwd()
GITDIR = subprocess.check_output(['git', 'rev-parse', '--absolute-git-dir'], text=True).strip()
def output(root):
    return root.parent / (root.name + '-results')


def report_path(root, contract='wait_time-v1'):
    return output(root) / (contract + '.json')


def edit(root, path, old, new):
    target = root / path
    text = target.read_text()
    assert text.count(old) == 1, (path, old)
    target.write_text(text.replace(old, new, 1))


def run(root):
    result = subprocess.run(['bash', 'scripts/core-status.sh'], cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=1800,
                            env=dict(os.environ, PGWT_CORE_STATUS_DIR=str(output(root))))
    assert report_path(root).is_file(), result.stdout
    report = json.loads(report_path(root).read_text())
    return result, report


with tempfile.TemporaryDirectory(prefix='pgwt-core-status-') as tmp:
    work = Path(tmp)
    base = work / 'baseline'
    for directory in ('src/core', 'contracts/wait_time-v1'):
        shutil.copytree(ROOT / directory, base / directory)
    for filename in ('scripts/core-status.sh', 'scripts/tree-hash.sh',
                     'scripts/core_status_contracts.py', 'tests/core_mutants.sh', 'tests/test_core_wait_time_contract.c',
                     'tests/test_core_wait_time_differential.py', '.gitignore'):
        target = base / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / filename, target)
    # Share read-only Git metadata. core-status isolates tree-hash writes in
    # temporary storage and never changes this checkout's index or object store.
    (base / '.git').write_text('gitdir: ' + GITDIR + '\n')
    # Existing wait-time controls remain real compiler/test runs. New-contract
    # controls below exercise the complete production entry point separately.
    edit(base, 'scripts/core-status.sh',
         '    other_reports = run_contracts(ROOT, OUT, provenance)',
         '    other_reports = []')
    result, baseline = run(base)
    assert result.returncode == 0 and baseline['state'] == 'PASS', result.stdout
    assert baseline['claims'] and all(p['state'] == 'PASS' for p in baseline['proofs'].values())
    assert len(baseline['commit']) == 40 and baseline['tree_hash'] and baseline['contract_sha256']

    for name in ('renamed-diff', 'changed-mutant', 'stale-json', 'empty-claims', 'unclaimed-mutant'):
        target = work / name
        shutil.copytree(base, target)
        if name == 'renamed-diff':
            # The comparator still exits zero, but no longer reaches the frozen id.
            edit(target, 'tests/test_core_wait_time_differential.py',
                 '        cases = generate()\n',
                 '        cases = generate()\n        cases[0]["id"] = "renamed_in_test"\n')
        elif name == 'changed-mutant':
            # Equivalent wrong predicate still kills the intended case. A
            # names-only KILLED parser would incorrectly certify this drift.
            edit(target, 'tests/core_mutants.sh',
                 '            source.write_text(text.replace(old, new, 1))\n',
                 '            applied = new.replace("w.end <= q->to", "!(w.end > q->to)") if name == "M1" else new\n'
                 '            source.write_text(text.replace(old, applied, 1))\n')
        elif name == 'stale-json':
            output(target).mkdir()
            shutil.copy2(report_path(base), report_path(target))
            assert json.loads(report_path(target).read_text())['state'] == 'PASS'
            edit(target, 'src/core/wait_metrics.c',
                 'time->value.present = !coverage_gap;', 'time->value.present = true;')
        elif name == 'empty-claims':
            (target / 'contracts/wait_time-v1/CLAIMS.tsv').write_text('')
        else:
            table = target / 'contracts/wait_time-v1/mutants.json'
            mutants = json.loads(table.read_text())
            mutants.append({**mutants[0], 'id': 'UNCLAIMED'})
            table.write_text(json.dumps(mutants) + '\n')
        result, report = run(target)
        assert result.returncode != 0 and report['state'] in ('FAIL', 'NOT-RUN'), (name, result.stdout)
        if name == 'renamed-diff':
            assert report['proofs']['diff:A1_missing']['state'] == 'NOT-RUN', report
            assert report['runs'][0]['exit_code'] == 0, report
        elif name == 'changed-mutant':
            assert report['proofs']['mutant:M1']['state'] == 'FAIL', report
            assert 'M1 KILLED (contract wait_window_02,' in (output(target) / 'mutants.log').read_text()
            assert (target / 'contracts/wait_time-v1/mutants.json').read_bytes() == (base / 'contracts/wait_time-v1/mutants.json').read_bytes()
        elif name == 'stale-json':
            assert report['proofs']['case:wait_window_08.json']['state'] == 'FAIL', report
            assert report['tree_hash'] != baseline['tree_hash'], report
            assert report['contract_sha256'] == baseline['contract_sha256'], report
        elif name == 'empty-claims':
            assert not report['claims'] and report['errors'], report
        else:
            assert 'unreferenced proof id: mutant:UNCLAIMED' in report['errors'], report
            assert report['proofs']['mutant:UNCLAIMED']['state'] == 'NOT-RUN', report
        print(f"core-status self-test {name}: {report['state']} (expected; never PASS)", flush=True)

    complete = work / 'complete'
    for directory in ('src/core', 'contracts/wait_time-v1', 'contracts/merge-v1',
                      'contracts/evidence-v1', 'tests/fixtures'):
        shutil.copytree(ROOT / directory, complete / directory)
    for filename in ('scripts/core-status.sh', 'scripts/core_status_contracts.py',
                     'scripts/core_status_ids.json', 'scripts/tree-hash.sh', '.gitignore'):
        target = complete / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / filename, target)
    for pattern in ('*.py', '*.c', '*.h', '*mutants.sh'):
        for path in (ROOT / 'tests').glob(pattern):
            shutil.copy2(path, complete / 'tests' / path.name)
    (complete / '.git').write_text('gitdir: ' + GITDIR + '\n')
    result, _ = run(complete)
    assert result.returncode == 0, result.stdout
    reports = {contract: json.loads(report_path(complete, contract).read_text())
               for contract in ('wait_time-v1', 'merge-v1', 'evidence-v1')}
    assert all(r['state'] == 'PASS' and r['errors'] == [] for r in reports.values()), result.stdout
    print('core-status self-test all contracts: real baseline PASS', flush=True)

    # Exercise real failing C baselines too: their records must be exposed
    # exactly once before a harness refuses to run mutants.
    sys_path = str(ROOT / 'scripts')
    import sys
    sys.path.insert(0, sys_path)
    from core_status_contracts import output_id
    baseline_failures = [
        ('merge', 'tests/test_core_merge_contract.c',
         '    if (field) printf("row %"',
         '    if (row_id == 3 && !strcmp(mode, "N")) field = "status_negative";\n    if (field) printf("row %"',
         ['bash', 'tests/core_merge_mutants.sh'], 'case:row-3/N'),
        ('evidence', 'tests/test_core_evidence_contract.c',
         '            int compared = evidence_compare(&input, &out, expected, projections, field, sizeof(field));',
         '            int compared = evidence_compare(&input, &out, expected, projections, field, sizeof(field));\n            if (revision == 0 && i == 7) compared = 1;',
         ['bash', 'tests/evidence_mutants.sh'], 'case:08_attach_failure'),
        ('codec', 'tests/test_core_evidence_codec.c',
         'static void check(const char *name, bool ok)\n{\n    if (!selected(name)) return;',
         'static void check(const char *name, bool ok)\n{\n    if (!strcmp(name, "golden_646_bytes")) ok = false;\n    if (!selected(name)) return;',
         ['python3', '-B', 'tests/test_evidence_codec_mutants.py'], 'case:codec/golden_646_bytes')]
    for label, filename, old, new, argv, proof_id in baseline_failures:
        failed = work / ('real-baseline-failure-' + label)
        shutil.copytree(complete, failed)
        edit(failed, filename, old, new)
        result = subprocess.run(argv, cwd=failed, capture_output=True, text=True, timeout=180)
        assert result.returncode != 0, result.stdout
        lines = [line for line in (result.stdout + result.stderr).splitlines()
                 if output_id(label, line) == proof_id]
        assert len(lines) == 1 and ('FAIL' in lines[0] or 'MISMATCH' in lines[0]), (label, lines, result.stdout, result.stderr)
        print(f'core-status self-test {label} real failing baseline: one FAIL record', flush=True)

    # This run's RAW runner transcripts become fixture-runner responses for
    # parser/derivation controls. They are not saved status inputs and are
    # never used to certify product behavior. This avoids recompiling hundreds
    # of identical mutants for each status-parser negative control.
    fixtures = {}
    for report in reports.values():
        for executed in report['runs']:
            fixtures[json.dumps(executed['command'])] = {
                'code': executed['exit_code'],
                'stdout': (output(complete) / executed['log']).read_text()}
    fixture_path = work / 'runner-fixtures.json'
    fixture_path.write_text(json.dumps(fixtures))
    injected = """
_fixture_path = Path(os.environ['PGWT_STATUS_TEST_FIXTURES'])
_fixture_responses = json.loads(_fixture_path.read_text())
_fixture_original_run = subprocess.run

def _fixture_run(argv, **kwargs):
    key = json.dumps(argv)
    if key not in _fixture_responses:
        return _fixture_original_run(argv, **kwargs)
    item = _fixture_responses[key]
    if os.environ.get('PGWT_STATUS_TEST_CHANGE_TREE') == '1' and argv == ['bash', 'tests/core_merge_mutants.sh']:
        with (ROOT / 'src/core/merge.c').open('a') as changed:
            changed.write('\\n/* status test: tree changed during proof run */\\n')
    return subprocess.CompletedProcess(argv, item['code'], stdout=item['stdout'])

subprocess.run = _fixture_run
"""
    edit(complete, 'scripts/core-status.sh', 'ROOT = Path.cwd()', 'ROOT = Path.cwd()' + injected)
    os.environ['PGWT_STATUS_TEST_FIXTURES'] = str(fixture_path)

    def fixture_run(root, responses=None):
        fixture_path.write_text(json.dumps(responses or fixtures))
        result, _ = run(root)
        return result, {c: json.loads(report_path(root, c).read_text()) for c in reports}

    def states_from_proofs(report):
        for claim in report['claims']:
            states = [report['proofs'].get(ref, {'state': 'NOT-RUN'})['state'] for ref in claim['proof_ids']]
            expected = 'FAIL' if 'FAIL' in states else 'NOT-RUN' if 'NOT-RUN' in states else 'PASS'
            assert claim['state'] == expected, (claim['id'], claim['state'], expected)

    result, baseline = fixture_run(complete)
    assert result.returncode == 0, result.stdout
    for contract, proof_id in [('merge-v1', 'case:row-3/N'),
                               ('evidence-v1', 'case:08_attach_failure')]:
        line = baseline[contract]['proofs'][proof_id]['lines'][0]
        for name in ('failed-proof', 'malformed-proof', 'bare-proof', 'bad-then-pass', 'missing-proof', 'outside-baseline', 'duplicate-proof', 'unknown-output'):
            responses = json.loads(json.dumps(fixtures))
            matched = [item for item in responses.values() if line + '\n' in item['stdout']]
            assert len(matched) == 1
            item = matched[0]
            if name == 'failed-proof': replacement = line.replace(' PASS', ' FAIL assertion')
            elif name == 'malformed-proof': replacement = line.replace(' PASS', ' WRONG')
            elif name == 'bare-proof': replacement = line.rsplit(' PASS', 1)[0]
            elif name == 'bad-then-pass': replacement = line.replace(' PASS', ' WRONG') + '\n' + line
            elif name in ('missing-proof', 'outside-baseline'): replacement = ''
            elif name == 'duplicate-proof': replacement = line + '\n' + line
            else: replacement = line.replace(proof_id.split(':', 1)[1].removeprefix('row-'), 'UNKNOWN_PROOF')
            item['stdout'] = item['stdout'].replace(line + '\n', replacement + '\n' if replacement else '')
            if name == 'outside-baseline':
                item['stdout'] += line + '\n'
            # Seed old green JSON; the next invocation must replace it.
            output(complete).mkdir(exist_ok=True)
            report_path(complete, contract).write_text(json.dumps(baseline[contract]))
            result, got = fixture_run(complete, responses)
            report = got[contract]
            assert result.returncode != 0 and report['state'] != 'PASS', (name, result.stdout)
            states_from_proofs(report)
            if name in ('failed-proof', 'malformed-proof', 'bare-proof', 'bad-then-pass', 'duplicate-proof'):
                assert report['proofs'][proof_id]['state'] == 'FAIL'
                affected = {c['id'] for c in baseline[contract]['claims'] if proof_id in c['proof_ids']}
                assert {c['id'] for c in report['claims'] if c['state'] == 'FAIL'} == affected
                assert all(c['state'] == 'PASS' for c in report['claims'] if c['id'] not in affected)
            elif name in ('missing-proof', 'outside-baseline'):
                assert report['proofs'][proof_id]['state'] == 'NOT-RUN'
            else:
                assert any('unknown proof id in output:' in error for error in report['errors'])
            assert all(r['state'] == 'PASS' for c, r in got.items() if c != contract)
            print(f'core-status self-test {contract} {name}: caught', flush=True)

        mutation_id = 'mutant:M1' if contract == 'merge-v1' else 'mutant:codec/C01_crc_polynomial'
        mutation_line = baseline[contract]['proofs'][mutation_id]['lines'][0]
        responses = json.loads(json.dumps(fixtures))
        for item in responses.values():
            item['stdout'] = item['stdout'].replace(mutation_line, re.sub(r'(source-sha256=)([0-9a-f])', lambda m: m[1] + ('0' if m[2] != '0' else '1'), mutation_line))
        result, got = fixture_run(complete, responses)
        assert result.returncode != 0 and got[contract]['proofs'][mutation_id]['state'] == 'FAIL'
        states_from_proofs(got[contract])
        print(f'core-status self-test {contract} changed mutation source hash: caught', flush=True)

        mutation_ids = ['mutant:M1'] if contract == 'merge-v1' else [
            'mutant:replay/E1_no_attach', 'mutant:codec/C01_crc_polynomial',
            'mutant:report/first_fault', 'equivalent:U428_query_window']
        for ident in mutation_ids:
            good = baseline[contract]['proofs'][ident]['lines'][0]
            prefix = good.split(' KILLED', 1)[0] if ' KILLED' in good else good.split(' | ', 1)[0]
            for malformed in (prefix, prefix + ' WRONG\n' + good):
                responses = json.loads(json.dumps(fixtures))
                for item in responses.values():
                    item['stdout'] = item['stdout'].replace(good + '\n', malformed + '\n')
                result, got = fixture_run(complete, responses)
                assert result.returncode != 0 and got[contract]['proofs'][ident]['state'] == 'FAIL', (ident, malformed, result.stdout)
                states_from_proofs(got[contract])
            print(f'core-status self-test {contract} malformed mutation/equivalence record {ident}: caught', flush=True)

        if contract == 'merge-v1':
            good = baseline[contract]['proofs']['mutant:M1']['lines'][0]
            wrong = good.replace('row 3/N MISMATCH kind', 'row 1/N MISMATCH kind').replace('intended-proof=case:row-3/N', 'intended-proof=case:row-1/N')
            assert wrong != good
            responses = json.loads(json.dumps(fixtures))
            for item in responses.values():
                item['stdout'] = item['stdout'].replace(good, wrong)
            result, got = fixture_run(complete, responses)
            assert result.returncode != 0 and got[contract]['proofs']['mutant:M1']['state'] == 'FAIL'
            print('core-status self-test merge-v1 wrong intended row/assertion: caught', flush=True)

        frame = 'MERGE' if contract == 'merge-v1' else 'EVIDENCE'
        for name in ('missing-baseline', 'duplicate-baseline', 'unfinished-baseline'):
            responses = json.loads(json.dumps(fixtures))
            matches = [item for item in responses.values() if frame + ' BASELINE BEGIN\n' in item['stdout']]
            assert len(matches) == 1
            item = matches[0]
            if name == 'missing-baseline':
                item['stdout'] = item['stdout'].replace(frame + ' BASELINE BEGIN\n', '')
            elif name == 'duplicate-baseline':
                item['stdout'] = item['stdout'].replace(frame + ' BASELINE BEGIN\n', (frame + ' BASELINE BEGIN\n') * 2)
            else:
                item['stdout'] = item['stdout'].replace(frame + ' BASELINE END\n', '')
            result, got = fixture_run(complete, responses)
            assert result.returncode != 0 and any('missing, duplicate or unfinished contract baseline' in e for e in got[contract]['errors'])
            print(f'core-status self-test {contract} {name}: caught', flush=True)

        for name in ('unknown-reference', 'unreferenced-proof', 'empty-claims'):
            case = work / (contract + '-' + name)
            shutil.copytree(complete, case)
            claims = case / 'contracts' / contract / 'CLAIMS.tsv'
            if name == 'unknown-reference':
                text = claims.read_text().splitlines()
                row = next(i for i, line in enumerate(text) if line and not line.startswith('#') and not line.startswith('claim_id'))
                text[row] += ' case:DOES_NOT_EXIST'
                claims.write_text('\n'.join(text) + '\n')
            elif name == 'unreferenced-proof':
                claims.write_text(claims.read_text().replace(proof_id + ' ', '').replace(' ' + proof_id, ''))
            else:
                claims.write_text('')
            result, got = fixture_run(case)
            assert result.returncode != 0 and got[contract]['state'] != 'PASS', result.stdout
            expected_error = ('unknown proof id: case:DOES_NOT_EXIST' if name == 'unknown-reference' else
                              'unreferenced proof id: ' + proof_id if name == 'unreferenced-proof' else 'implementation-on-master')
            assert any(expected_error in e for e in got[contract]['errors']), got[contract]['errors']
            print(f'core-status self-test {contract} {name}: caught', flush=True)

        # Seed a broken status derivation; a missing proof MUST expose its
        # falsely green claim to the very same checker used above.
        mutant = work / (contract + '-green-without-proof')
        shutil.copytree(complete, mutant)
        edit(mutant, 'scripts/core_status_contracts.py',
             "        claim['state'] = 'FAIL' if 'FAIL' in states else 'NOT-RUN' if 'NOT-RUN' in states else 'PASS'",
             "        claim['state'] = 'PASS'")
        responses = json.loads(json.dumps(fixtures))
        for item in responses.values():
            item['stdout'] = item['stdout'].replace(line + '\n', '')
        _, got = fixture_run(mutant, responses)
        try:
            states_from_proofs(got[contract])
        except AssertionError:
            print(f'core-status self-test {contract} seeded green-without-proof mutant: KILLED', flush=True)
        else:
            raise AssertionError(contract + ' status mutant survived')

    changed = work / 'changed-during-run'
    shutil.copytree(complete, changed)
    os.environ['PGWT_STATUS_TEST_CHANGE_TREE'] = '1'
    result, got = fixture_run(changed)
    del os.environ['PGWT_STATUS_TEST_CHANGE_TREE']
    assert result.returncode != 0
    for report in got.values():
        assert 'tree changed during proof run; discarding evidence' in report['errors'], report['errors']
        assert report['proofs'] and all(p['state'] == 'NOT-RUN' for p in report['proofs'].values())
        assert all(c['state'] == 'NOT-RUN' for c in report['claims'])
    print('core-status self-test changed tree: all contracts discarded', flush=True)
    del os.environ['PGWT_STATUS_TEST_FIXTURES']
    print('core-status self-test: PASS', flush=True)
PY
