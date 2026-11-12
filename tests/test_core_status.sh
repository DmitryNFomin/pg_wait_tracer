#!/usr/bin/env bash
# Real runners in temporary checkout copies: no edits to the invoking tree.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - <<'PY'
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path.cwd()
GITDIR = subprocess.check_output(['git', 'rev-parse', '--absolute-git-dir'], text=True).strip()
REPORT = Path('tests/results/core_status/wait_time-v1.json')


def edit(root, path, old, new):
    target = root / path
    text = target.read_text()
    assert text.count(old) == 1, (path, old)
    target.write_text(text.replace(old, new, 1))


def run(root):
    result = subprocess.run(['bash', 'scripts/core-status.sh'], cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=240)
    assert (root / REPORT).is_file(), result.stdout
    report = json.loads((root / REPORT).read_text())
    return result, report


with tempfile.TemporaryDirectory(prefix='pgwt-core-status-') as tmp:
    work = Path(tmp)
    base = work / 'baseline'
    for directory in ('src/core', 'contracts/wait_time-v1'):
        shutil.copytree(ROOT / directory, base / directory)
    for filename in ('scripts/core-status.sh', 'scripts/tree-hash.sh',
                     'tests/core_mutants.sh', 'tests/test_core_wait_time_contract.c',
                     'tests/test_core_wait_time_differential.py', '.gitignore'):
        target = base / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / filename, target)
    # Share read-only Git metadata. core-status isolates tree-hash writes in
    # temporary storage and never changes this checkout's index or object store.
    (base / '.git').write_text('gitdir: ' + GITDIR + '\n')
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
            assert json.loads((target / REPORT).read_text())['state'] == 'PASS'
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
            assert 'M1 KILLED (contract wait_window_02,' in (target / 'tests/results/core_status/mutants.log').read_text()
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
PY
