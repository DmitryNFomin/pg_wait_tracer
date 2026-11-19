#!/usr/bin/env bash
# Real proofs build in temporary directories; controls edit checkout copies.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - "$@" <<'PY'
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import time

ROOT = Path.cwd()
GITDIR = subprocess.check_output(['git', 'rev-parse', '--absolute-git-dir'], text=True).strip()
sys.path.insert(0, str(ROOT / 'scripts'))
from core_status_contracts import contract_hash, inventories, tree_hash


def transcripts(directory, expected_tree):
    """Admit only complete, current-tree logs, never saved claim states.

    The JSON is a provenance/completeness envelope. The returned values are
    raw runner output; every state is derived again by the fixture runs.
    """
    definitions, commands = inventories(ROOT)
    wait = ROOT / 'contracts/wait_time-v1'
    expected_proofs = {contract: set(proofs) for contract, proofs in definitions.items()}
    expected_proofs['wait_time-v1'] = (
        {'case:' + p.name for p in (wait / 'contracts/examples').glob('*.json')} |
        {'mutant:' + row['id'] for row in json.loads((wait / 'mutants.json').read_text())} |
        {'diff:' + row['id'] for row in json.loads((wait / 'differential_cases.json').read_text())})
    commands = {'wait_time-v1': [('differential', ['python3', '-B', 'tests/test_core_wait_time_differential.py']),
                               ('mutants', ['bash', 'tests/core_mutants.sh'])], **commands}
    fixtures = {}
    for contract, runners in commands.items():
        report = json.loads((directory / (contract + '.json')).read_text())
        if report['tree_hash'] != expected_tree:
            raise ValueError('transcript tree mismatch: ' + contract)
        expected = [(argv, ('' if contract == 'wait_time-v1' else contract + '-') + label + '.log')
                    for label, argv in runners]
        actual = [(run['command'], run['log']) for run in report['runs']]
        if (report['contract'] != contract or report['state'] != 'PASS' or report['errors'] or
                not report['claims'] or set(report['proofs']) != expected_proofs[contract] or
                any(p['state'] != 'PASS' for p in report['proofs'].values()) or actual != expected or
                report['contract_sha256'] != contract_hash(ROOT / 'contracts' / contract)):
            raise ValueError('incomplete transcript run: ' + contract)
        for run in report['runs']:
            raw = (directory / run['log']).read_text()
            if run['exit_code'] != 0 or hashlib.sha256(raw.encode()).hexdigest() != run['log_sha256']:
                raise ValueError('incomplete or changed transcript: ' + run['log'])
            fixtures[json.dumps(run['command'])] = {'code': run['exit_code'], 'stdout': raw}
    return fixtures


if len(sys.argv) > 2:
    raise SystemExit('usage: test_core_status.sh [this-invocation-status-directory]')
# Validate before starting any runner, including the wait-time controls.
supplied = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else None
expected_tree = tree_hash(ROOT)
try:
    supplied_fixtures = transcripts(supplied, expected_tree) if supplied else None
except (OSError, ValueError, KeyError, TypeError) as error:
    raise SystemExit('core-status self-test REFUSED: ' + str(error))


def output(root):
    return (root.parent / (root.name + '-results')).resolve()


def report_path(root, contract='wait_time-v1'):
    return output(root) / (contract + '.json')


def edit(root, path, old, new):
    target = root / path
    text = target.read_text()
    assert text.count(old) == 1, (path, old)
    target.write_text(text.replace(old, new, 1))


def run(root, destination=None):
    directory = destination if destination is not None else output(root)
    if root == ROOT:
        print('core-status self-test own plain baseline uptime before: ' +
              subprocess.check_output(['uptime'], text=True).strip(), flush=True)
    started = time.monotonic()
    result = subprocess.run(['bash', 'scripts/core-status.sh'], cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=1800,
                            env=dict(os.environ, PGWT_CORE_STATUS_DIR=str(directory)))
    if root == ROOT:
        print(f'core-status self-test own plain baseline seconds: {time.monotonic() - started:.3f}', flush=True)
        print('core-status self-test own plain baseline uptime after: ' +
              subprocess.check_output(['uptime'], text=True).strip(), flush=True)
        print(result.stdout, end='', flush=True)
    path = directory / 'wait_time-v1.json'
    assert path.is_file(), result.stdout
    report = json.loads(path.read_text())
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
            assert 'CORE-STATUS PROOF wait_time-v1:diff:A1_missing NOT-RUN claims=' in result.stdout
            assert 'log=' + str(output(target) / 'differential.log') in result.stdout
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
    if supplied is None:
        # This is also the exact plain CI command, on the invoking tree;
        # only its output goes to temporary storage. No product code is edited.
        result, _ = run(ROOT, output(complete))
        assert result.returncode == 0, result.stdout
        source = output(complete)
        fixtures = transcripts(source, expected_tree)
        print('core-status self-test all contracts: own real baseline PASS', flush=True)
    else:
        if tree_hash(ROOT) != expected_tree:
            raise SystemExit('core-status self-test REFUSED: tree changed before replay')
        source, fixtures = supplied, supplied_fixtures
        print('core-status self-test all contracts: this invocation baseline accepted', flush=True)
    reports = {contract: json.loads((source / (contract + '.json')).read_text())
               for contract in ('wait_time-v1', 'merge-v1', 'evidence-v1')}

    # Refusal happens before fixture injection/replay. Cover both provenance
    # and completeness independently, plus a log truncated after JSON write.
    source_tree = reports['wait_time-v1']['tree_hash']
    for name in ('other-tree', 'missing-contract', 'missing-proof', 'missing-runner', 'failed-runner', 'truncated-log'):
        rejected = work / ('refused-' + name)
        shutil.copytree(source, rejected)
        path = rejected / 'wait_time-v1.json'
        envelope = json.loads(path.read_text())
        if name == 'other-tree': envelope['tree_hash'] = '0' * 40
        elif name == 'missing-contract': (rejected / 'evidence-v1.json').unlink()
        elif name == 'missing-proof': envelope['proofs'].pop(next(iter(envelope['proofs'])))
        elif name == 'missing-runner': envelope['runs'].pop()
        elif name == 'failed-runner': envelope['runs'][0]['exit_code'] = 1
        else: (rejected / envelope['runs'][0]['log']).write_text('truncated\n')
        path.write_text(json.dumps(envelope))
        try:
            transcripts(rejected, source_tree)
        except (OSError, ValueError) as error:
            print(f'core-status self-test {name}: REFUSED before replay ({error})', flush=True)
        else:
            raise AssertionError(name + ' transcripts were accepted')

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
        for name in ('failed-proof', 'malformed-proof', 'bare-proof', 'bad-then-pass', 'bare-then-pass', 'missing-proof', 'runner-failed', 'outside-baseline', 'duplicate-proof', 'unknown-output'):
            responses = json.loads(json.dumps(fixtures))
            matched = [item for item in responses.values() if line + '\n' in item['stdout']]
            assert len(matched) == 1
            item = matched[0]
            if name == 'failed-proof': replacement = line.replace(' PASS', ' FAIL assertion')
            elif name == 'malformed-proof': replacement = line.replace(' PASS', ' WRONG')
            elif name == 'bare-proof': replacement = line.rsplit(' PASS', 1)[0]
            elif name == 'bad-then-pass': replacement = line.replace(' PASS', ' WRONG') + '\n' + line
            elif name == 'bare-then-pass': replacement = line.rsplit(' PASS', 1)[0] + '\n' + line
            elif name in ('missing-proof', 'runner-failed', 'outside-baseline'): replacement = ''
            elif name == 'duplicate-proof': replacement = line + '\n' + line
            else: replacement = line.replace(proof_id.split(':', 1)[1].removeprefix('row-'), 'UNKNOWN_PROOF')
            item['stdout'] = item['stdout'].replace(line + '\n', replacement + '\n' if replacement else '')
            if name == 'outside-baseline':
                item['stdout'] += line + '\n'
            if name == 'runner-failed':
                item['code'] = 1
                item['stdout'] += 'ERROR injected runner failure\n'
            # Seed old green JSON; the next invocation must replace it.
            output(complete).mkdir(exist_ok=True)
            report_path(complete, contract).write_text(json.dumps(baseline[contract]))
            result, got = fixture_run(complete, responses)
            report = got[contract]
            assert result.returncode != 0 and report['state'] != 'PASS', (name, result.stdout)
            states_from_proofs(report)
            if name in ('failed-proof', 'malformed-proof', 'bare-proof', 'bad-then-pass', 'bare-then-pass', 'duplicate-proof'):
                assert report['proofs'][proof_id]['state'] == 'FAIL'
                affected = {c['id'] for c in baseline[contract]['claims'] if proof_id in c['proof_ids']}
                assert {c['id'] for c in report['claims'] if c['state'] == 'FAIL'} == affected
                assert all(c['state'] == 'PASS' for c in report['claims'] if c['id'] not in affected)
            elif name in ('missing-proof', 'runner-failed', 'outside-baseline'):
                assert report['proofs'][proof_id]['state'] == 'NOT-RUN'
            else:
                assert any('unknown proof id in output:' in error for error in report['errors'])
            assert all(r['state'] == 'PASS' for c, r in got.items() if c != contract)
            if name == 'bare-then-pass':
                assert 'CORE-STATUS FIRST ' + replacement.splitlines()[0] + '\n' in result.stdout, result.stdout
            if name in ('failed-proof', 'missing-proof', 'runner-failed'):
                affected = [c['id'] for c in report['claims'] if proof_id in c['proof_ids']]
                label = 'merge' if contract == 'merge-v1' else 'evidence'
                diagnostic = (f'CORE-STATUS PROOF {contract}:{proof_id} '
                              f"{report['proofs'][proof_id]['state']} claims={','.join(affected)} "
                              f'log={output(complete) / (contract + "-" + label + ".log")}')
                assert diagnostic in result.stdout, result.stdout
                if name == 'failed-proof':
                    assert 'CORE-STATUS FIRST ' + replacement in result.stdout, result.stdout
                    print(diagnostic + '\nCORE-STATUS FIRST ' + replacement, flush=True)
                if name == 'runner-failed':
                    assert 'CORE-STATUS FIRST ERROR injected runner failure' in result.stdout, result.stdout
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

    # The merge index and queue runners have no baseline frame. Their per-id
    # records go through the same derivation; failure records use the runners'
    # own MISMATCH spelling.
    for proof_id, failure, mutation_id in (
            ('case:index/work', 'index work MISMATCH work lookup visits <= 64 at input 0',
             'mutant:index/linear_scan'),
            ('diff:queue/a95a214c', 'queue corpus MISMATCH output at input 7',
             'mutant:queue/skipped_suffix')):
        line = baseline['merge-v1']['proofs'][proof_id]['lines'][0]
        affected = {c['id'] for c in baseline['merge-v1']['claims'] if proof_id in c['proof_ids']}
        for name in ('failed-proof', 'missing-proof', 'duplicate-proof'):
            responses = json.loads(json.dumps(fixtures))
            matched = [item for item in responses.values() if line + '\n' in item['stdout']]
            assert len(matched) == 1
            replacement = {'failed-proof': failure + '\n', 'missing-proof': '',
                           'duplicate-proof': line + '\n' + line + '\n'}[name]
            matched[0]['stdout'] = matched[0]['stdout'].replace(line + '\n', replacement)
            result, got = fixture_run(complete, responses)
            report = got['merge-v1']
            assert result.returncode != 0 and report['state'] != 'PASS', (name, result.stdout)
            states_from_proofs(report)
            expected = 'NOT-RUN' if name == 'missing-proof' else 'FAIL'
            assert report['proofs'][proof_id]['state'] == expected, (name, report['proofs'][proof_id])
            assert {c['id'] for c in report['claims'] if c['state'] != 'PASS'} == affected
            assert all(r['state'] == 'PASS' for c, r in got.items() if c != 'merge-v1')
            print(f'core-status self-test merge-v1 {proof_id} {name}: caught', flush=True)
        good = baseline['merge-v1']['proofs'][mutation_id]['lines'][0]
        responses = json.loads(json.dumps(fixtures))
        for item in responses.values():
            item['stdout'] = item['stdout'].replace(good, re.sub(
                r'(source-sha256=)([0-9a-f])', lambda m: m[1] + ('0' if m[2] != '0' else '1'), good))
        result, got = fixture_run(complete, responses)
        assert result.returncode != 0 and got['merge-v1']['proofs'][mutation_id]['state'] == 'FAIL'
        states_from_proofs(got['merge-v1'])
        print(f'core-status self-test merge-v1 {mutation_id} changed mutation source hash: caught', flush=True)

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
