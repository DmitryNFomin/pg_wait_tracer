#!/usr/bin/env bash
# Status is regenerated from this run's per-id evidence; old JSON is never input.
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f tests/results/core_status/wait_time-v1.json
exec python3 -B - <<'PY'
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path.cwd()
CONTRACT = ROOT / "contracts/wait_time-v1"
OUT = ROOT / "tests/results/core_status"
OUT.mkdir(parents=True, exist_ok=True)
errors = []
claims = []
proofs = {}
runs = []


def contract_hash():
    digest = hashlib.sha256()
    for path in sorted(CONTRACT.rglob("*")):
        if path.is_file():
            digest.update(path.relative_to(CONTRACT).as_posix().encode() + b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def command(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, cwd=ROOT)
    if result.returncode:
        # A harness failure also refuses the gate, but cannot certify or mark a
        # claim: claim states below are derived exclusively from per-id records.
        errors.append(f"command failed ({result.returncode}): {' '.join(command)}")
    return result


def identify():
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    # The status needs the hash only, while other tree-hash callers retain the
    # object for git archive. Keep our read-only storage local to this call.
    objects = subprocess.check_output(["git", "rev-parse", "--git-path", "objects"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix='pgwt-status-objects-') as temporary:
        environment = os.environ.copy()
        environment['GIT_OBJECT_DIRECTORY'] = temporary
        alternates = environment.get('GIT_ALTERNATE_OBJECT_DIRECTORIES', '')
        environment['GIT_ALTERNATE_OBJECT_DIRECTORIES'] = str(Path(objects).resolve()) + (
            ':' + alternates if alternates else '')
        tree = subprocess.check_output(["bash", "scripts/tree-hash.sh"],
                                       text=True, env=environment).strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", tree):
        raise ValueError("invalid tree-hash output")
    return {"commit": commit, "tree_hash": tree, "contract_sha256": contract_hash()}


def inventory(path, fields):
    rows = json.loads(path.read_text())
    if not isinstance(rows, list) or not rows:
        raise ValueError(f"empty or invalid inventory: {path.name}")
    found = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != set(fields):
            raise ValueError(f"invalid row in {path.name}")
        ident = row['id']
        if not isinstance(ident, str) or not re.fullmatch(r"[A-Za-z0-9_]+", ident) or ident in found:
            raise ValueError(f"invalid or duplicate id in {path.name}: {ident}")
        found[ident] = row
    return found


def observe(ident, state, line):
    if ident not in proofs:
        errors.append(f"unknown proof id in output: {ident}")
        return
    proof = proofs[ident]
    # Duplicate records, even two positives, are ambiguous evidence.
    proof['state'] = state if not proof['lines'] else 'FAIL'
    proof['lines'].append(line)


def execute(label, argv):
    result = command(argv)
    (OUT / (label + '.log')).write_text(result.stdout)
    runs.append({'command': argv, 'exit_code': result.returncode,
                 'log': label + '.log',
                 'log_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
    return result.stdout.splitlines()


provenance = {}
try:
    provenance = identify()
    named = inventory(CONTRACT / 'differential_cases.json', ('id', 'evidence', 'query'))
    mutants = inventory(CONTRACT / 'mutants.json', ('id', 'original', 'replacement', 'intended_case'))
    examples = sorted((CONTRACT / 'contracts/examples').glob('*.json'))
    if not examples:
        raise ValueError('zero example cases')
    ids = (['case:' + path.name for path in examples] +
           ['mutant:' + ident for ident in mutants] + ['diff:' + ident for ident in named])
    proofs = {ident: {'state': 'NOT-RUN', 'lines': []} for ident in ids}
    for mutant in mutants.values():
        if (not all(isinstance(mutant[k], str) for k in mutant) or
                mutant['intended_case'] not in proofs or
                not mutant['intended_case'].startswith(('case:', 'diff:'))):
            raise ValueError(f"invalid intended case for mutant {mutant['id']}")
    lines = (CONTRACT / 'CLAIMS.tsv').read_text().splitlines()
    if not any(line and not line.startswith('#') for line in lines):
        raise ValueError('zero claims')
    if lines.count('# implementation-on-master: true') != 1 or any(
            line.startswith('# implementation-on-master:') and line != '# implementation-on-master: true'
            for line in lines):
        raise ValueError('implementation-on-master flag must be true exactly once')
    rows = csv.DictReader([line for line in lines if line and not line.startswith('#')], delimiter='\t')
    if rows.fieldnames != ['claim_id', 'claim', 'proof_ids']:
        raise ValueError('invalid CLAIMS.tsv header')
    used = set()
    for row in rows:
        ident, claim, refs = row['claim_id'], row['claim'], (row['proof_ids'] or '').split()
        if (None in row or not ident or not claim or not refs or
                any(c['id'] == ident for c in claims) or len(refs) != len(set(refs))):
            raise ValueError('invalid or duplicate claim')
        claims.append({'id': ident, 'claim': claim, 'proof_ids': refs})
        for ref in refs:
            if ref not in proofs:
                errors.append(f'unknown proof id: {ref}')
        used.update(refs)
    if not claims:
        errors.append('zero claims')
    for ident in sorted(set(proofs) - used):
        errors.append(f'unreferenced proof id: {ident}')

    # Validate the complete closed vocabulary before running any test.
    if not errors:
        for line in execute('differential', ['python3', '-B', 'tests/test_core_wait_time_differential.py']):
            match = re.fullmatch(r'NAMED (\S+) (MATCH|MISMATCH)', line)
            if match:
                observe('diff:' + match[1], 'PASS' if match[2] == 'MATCH' else 'FAIL', line)

        lines = execute('mutants', ['bash', 'tests/core_mutants.sh'])
        in_baseline = False
        baseline_count = 0
        for line in lines:
            if line == 'CORE CONTRACT BEGIN':
                baseline_count += 1
                in_baseline = True
                continue
            if line == 'CORE CONTRACT END':
                in_baseline = False
                continue
            if in_baseline:
                match = re.fullmatch(r'(wait_window_\d+\.json) (PASS|MISMATCH|ERROR)(?: \(.*\))?', line)
                if match:
                    observe('case:' + match[1], 'PASS' if line == match[1] + ' PASS' else 'FAIL', line)
                continue
            match = re.match(r'(\S+) (KILLED|PATCH_ERROR|BUILD_ERROR|TIMEOUT|SURVIVED/ERROR)\b', line)
            if not match:
                continue
            ident, outcome = match[1], match[2]
            state = 'FAIL'
            if ident in mutants and outcome == 'KILLED':
                mutant = mutants[ident]
                expected_hash = hashlib.sha256(json.dumps(
                    [mutant[k] for k in ('id', 'original', 'replacement', 'intended_case')],
                    separators=(',', ':')).encode()).hexdigest()
                source = (ROOT / 'src/core/wait_metrics.c').read_text()
                source_hash = hashlib.sha256(source.replace(
                    mutant['original'], mutant['replacement'], 1).encode()).hexdigest()
                case = mutant['intended_case']
                target = ('contract ' + case[5:-5] if case.startswith('case:')
                          else 'differential ' + case[5:])
                detail = 'case mismatch' if case.startswith('case:') else 'reference mismatch'
                expected = (f'{ident} KILLED ({target}, build OK, {detail}) '
                            f'definition-sha256={expected_hash} source-sha256={source_hash}')
                if source.count(mutant['original']) == 1 and line == expected:
                    state = 'PASS'
            observe('mutant:' + ident, state, line)
        if in_baseline or baseline_count != 1:
            errors.append('missing, duplicate or unfinished contract baseline')
        if identify() != provenance:
            errors.append('tree changed during proof run; discarding evidence')
            for proof in proofs.values():
                proof['state'] = 'NOT-RUN'
except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
    errors.append(str(error))

for claim in claims:
    states = [proofs.get(ref, {'state': 'NOT-RUN'})['state'] for ref in claim['proof_ids']]
    claim['state'] = 'FAIL' if 'FAIL' in states else 'NOT-RUN' if 'NOT-RUN' in states else 'PASS'
    print(f"CORE-STATUS {claim['id']} {claim['state']}")
state = 'FAIL' if errors or any(c['state'] == 'FAIL' for c in claims) else (
    'NOT-RUN' if not claims or any(c['state'] == 'NOT-RUN' for c in claims) else 'PASS')
report = {'contract': 'wait_time-v1', **provenance, 'state': state,
          'claims': claims, 'proofs': proofs, 'runs': runs, 'errors': errors}
# Replacement prevents a partial JSON file from looking like a completed run.
temporary = OUT / 'wait_time-v1.json.tmp'
temporary.write_text(json.dumps(report, indent=2) + '\n')
temporary.replace(OUT / 'wait_time-v1.json')
for error in errors:
    print('CORE-STATUS ERROR ' + error, file=sys.stderr)
sys.exit(0 if state == 'PASS' else 1)
PY
