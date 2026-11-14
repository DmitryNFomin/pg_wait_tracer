"""Per-run merge/evidence proofs; inventories contain identifiers, never status.

The shell runners keep their mutation registries before their execution entry
points. Load that declarative prelude, not main(), to bind each kill to exactly
its registered definition, patched source and intended assertion.
"""
import ast
import csv
import hashlib
import importlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def digest(value):
    return hashlib.sha256(json.dumps(value, separators=(',', ':')).encode()).hexdigest()


def registry(root, filename, stop):
    text = (root / filename).read_text()
    if filename.endswith('.sh'):
        text = text.split("<<'", 1)[1].split('\n', 1)[1].rsplit('\n', 2)[0]
    tree = ast.parse(text)
    nodes = []
    for node in tree.body:
        if (isinstance(node, ast.FunctionDef) and node.name == stop or
                isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == stop for t in node.targets)):
            break
        nodes.append(node)
    namespace = {'__file__': str(root / filename), '__name__': 'status_registry'}
    argv = sys.argv
    try:
        sys.argv = [filename, str(root)]
        exec(compile(ast.Module(body=nodes, type_ignores=[]), filename, 'exec'), namespace)
    finally:
        sys.argv = argv
    return namespace


def inventories(root):
    sys.path.insert(0, str(root / 'tests'))
    result = {'merge-v1': {}, 'evidence-v1': {}}
    commands = {'merge-v1': [('merge', ['bash', 'tests/core_merge_mutants.sh'])],
                'evidence-v1': [('evidence', ['bash', 'tests/evidence_mutants.sh']),
                                ('oracle', ['python3', '-B', 'tests/test_evidence_codec_oracle.py']),
                                ('codec', ['python3', '-B', 'tests/test_evidence_codec_mutants.py']),
                                ('redelivery', ['python3', '-B', 'tests/test_evidence_codec_redelivery.py']),
                                ('report', ['python3', '-B', 'tests/test_evidence_report.py'])]}

    def add(contract, ident, label, pattern, target=None):
        if ident in result[contract]:
            raise ValueError('duplicate proof id: ' + ident)
        result[contract][ident] = {'runner': label, 'pass_pattern': pattern, 'intended': target}

    def exact(contract, ident, label, line, target=None):
        add(contract, ident, label, re.escape(line), target)

    def binding(definition, source):
        return ' definition-sha256=' + digest(definition) + ' source-sha256=' + hashlib.sha256(source.encode()).hexdigest()

    names = json.loads((root / 'scripts/core_status_ids.json').read_text())
    m = registry(root, 'tests/core_merge_mutants.sh', 'compiler')
    for row in json.loads((root / 'contracts/merge-v1/examples.json').read_text())['rows']:
        for mode in row['modes']:
            ident = f"row-{row['id']}/{mode}"
            exact('merge-v1', 'case:' + ident, 'merge', f"row {row['id']}/{mode} PASS")
    source = (root / 'tests/test_core_merge_contract.c').read_text()
    guards = re.search(r'guard_names\[\] = \{(.*?)\};', source, re.S)[1]
    for guard in re.findall(r'"([^"]+)"', guards):
        exact('merge-v1', 'case:guard/' + guard, 'merge', f'guard {guard} PASS')
    for name in ('safety', 'recovery'):
        exact('merge-v1', 'case:' + name, 'merge', f'core merge {name}: passed')
    targets = {name: (case, assertion) for name, _, case, assertion in m['guard_mutations']}
    for name, edits, explanation in m['mutations'] + [(n, e, c) for n, e, c, _ in m['guard_mutations']]:
        src = m['original_test'] if name == 'M10' else m['original']
        for old, new in edits:
            if src.count(old) != 1:
                raise ValueError(f'{name}: mutation anchor must occur once')
            src = src.replace(old, new, 1)
        definition = [name, edits, explanation, targets.get(name), m['fixture_targets'].get(name)]
        suffix = binding(definition, src)
        if name in targets:
            case, assertion = targets[name]
            detail = re.escape(f'guard {case} MISMATCH {assertion}')
            target = 'case:guard/' + case
        else:
            # Also pin the intended row/mode/assertion for older runners
            # that accept any frozen-row mismatch as a kill. These are proof
            # identifiers, not numerical expected answers.
            row, mode, field = m['fixture_targets'].get(name, names['merge_fixture_targets'].get(name, (None, None, None)))
            detail = (rf'row {row}/{mode} MISMATCH {re.escape(field)} at output .+' if row else
                      r'row [0-9]+/[NQ] MISMATCH .+')
            target = f'case:row-{row}/{mode}' if row else None
        pattern = re.escape(name + ' KILLED: ') + detail + re.escape(suffix + ' intended-proof=')
        pattern += re.escape(target) if target is not None else r'case:row-[0-9]+/[NQ]'
        add('merge-v1', 'mutant:' + name, 'merge', pattern, target)

    e = registry(root, 'tests/evidence_mutants.sh', 'main')
    diff = e['diff']
    scenario_ids = [json.loads(p.read_text())['id'] for directory in ('scenarios', 'r3/scenarios')
                    for p in sorted((root / 'contracts/evidence-v1' / directory).glob('*.json'))]
    for ident in scenario_ids:
        exact('evidence-v1', 'case:' + ident, 'evidence', f'evidence {ident} PASS')
    # Native/API and codec assertion names form a reviewed, closed vocabulary.
    for ident in names['native']:
        exact('evidence-v1', 'case:' + ident, 'evidence', f'evidence {ident} PASS')
    with tempfile.TemporaryDirectory(prefix='pgwt-status-inventory-') as tmp:
        _, _, expected, cases, _ = diff.prepare_corpus(Path(tmp))
    named = {m[3] for m in e['mutants'] + e['differential_mutants'] + e['guard_mutants']}
    for case, answer in zip(cases, expected):
        ident = case['id']
        if not ident.startswith('guard_') and ident not in named:
            continue
        outcome = 'unsupported' if 'error' in answer else answer['wait_time']['completeness']
        exact('evidence-v1', 'diff:' + ident, 'evidence',
              f'evidence differential {ident} PASS reference={outcome} core={outcome}')
    exact('evidence-v1', 'diff:seeded_suite', 'evidence', 'evidence differential seeded_suite PASS reference=MATCH core=MATCH')
    diff_names = {m[0] for m in e['differential_mutants'] + e['guard_mutants']} - e['contract_guards']
    for definition in e['mutants'] + e['differential_mutants'] + e['guard_mutants']:
        name, old, new, case, field = definition
        source = (root / e['mutation_sources'].get(name, 'src/core/evidence_replay.c')).read_text()
        if source.count(old) != 1:
            raise ValueError(f'{name}: mutation anchor must occur once')
        suffix = binding(definition, source.replace(old, new, 1))
        target = ('diff:' if name in diff_names else 'case:') + case
        suffix += ' intended-proof=' + target
        if name in e['equivalent_deletions']:
            pattern = re.escape(name + ' | ') + r'[0-9]+' + re.escape(
                ' | contract=0 differential=0 | SURVIVED (cannot change an answer): ' + e['equivalent_deletions'][name] + suffix)
            add('evidence-v1', 'equivalent:' + name, 'evidence', pattern)
        else:
            detail = re.escape(field) + (r'(?:[.\[].*)?' if name not in diff_names else '')
            extra = '; both result objects unchanged' if name == 'E8_shift_coverage' else ''
            pattern = re.escape(f'{name} KILLED ({case}, ') + detail + re.escape(', build OK' + extra + ')' + suffix)
            target = ('diff:' if name in diff_names else 'case:') + case
            add('evidence-v1', 'mutant:replay/' + name, 'evidence', pattern, target)

    codec = importlib.import_module('test_evidence_codec_mutants')
    original = (root / 'src/core/evidence_codec.c').read_text()
    for ident in names['codec']:
        exact('evidence-v1', 'case:codec/' + ident, 'codec', f'codec {ident} PASS')
    for definition in codec.MUTANTS + codec.GUARDS + codec.REDELIVERY_MUTATIONS:
        name, old, new, witness = definition
        if original.count(old) != 1:
            raise ValueError(f'{name}: mutation anchor must occur once')
        suffix = binding(definition, original.replace(old, new, 1))
        if definition in codec.REDELIVERY_MUTATIONS:
            exact('evidence-v1', 'case:probe/' + name, 'codec', f'codec probe_{name} PASS')
            line = f'codec {name} KILLED by redelivery_{witness} (built; intended independent assertion failed)' + suffix
            target = 'case:redelivery/' + witness[5:] if witness.startswith('case_') else 'case:probe/' + name
        else:
            witnesses = (witness,) if isinstance(witness, str) else witness
            line = f"codec {name} KILLED by {', '.join(witnesses)} (built; intended assertion failed)" + suffix
            target = 'case:codec/' + witnesses[0]
        line += ' intended-proof=' + target
        exact('evidence-v1', 'mutant:codec/' + name, 'codec', line, target)
    exact('evidence-v1', 'case:golden-bytes', 'oracle', 'codec independent_golden_bytes PASS')
    # The oracle's native invocation also prints this already existing assertion.
    exact('evidence-v1', 'case:oracle/golden_646_bytes', 'oracle', 'codec golden_646_bytes PASS')
    for n in range(1, 51):
        exact('evidence-v1', f'case:redelivery/{n}', 'redelivery', f'codec redelivery_case_{n} PASS')
        exact('evidence-v1', f'case:report/{n}', 'report', f'report case_{n} PASS')
    exact('evidence-v1', 'diff:redelivery-generated', 'redelivery', 'codec redelivery_generated PASS')
    for ident in scenario_ids[33:]:
        exact('evidence-v1', 'case:report/' + ident, 'report', f'report r3_{ident} PASS')
    exact('evidence-v1', 'case:report/api_errors', 'report', 'report api_errors PASS')
    exact('evidence-v1', 'case:report/two_faults', 'report', 'report two_faults PASS final=file[2].record[0] first=file[2].record[1]; independent x F=H=10')
    report = importlib.import_module('test_evidence_report')
    original = (root / 'src/core/evidence_replay.c').read_text()
    for definition in report.MUTANTS:
        name, old, new, witness, field = definition
        if original.count(old) != 1:
            raise ValueError(f'{name}: mutation anchor must occur once')
        line = f'report mutant {name}: KILLED case={witness} assertion={field}' + binding(definition, original.replace(old, new, 1))
        target = 'case:report/' + str(witness)
        exact('evidence-v1', 'mutant:report/' + name, 'report', line + ' intended-proof=' + target, target)
    return result, commands


def contract_hash(directory):
    h = hashlib.sha256()
    for path in sorted(directory.rglob('*')):
        if path.is_file():
            h.update(path.relative_to(directory).as_posix().encode() + b'\0')
            h.update(hashlib.sha256(path.read_bytes()).digest())
    return h.hexdigest()


def read_claims(path, proofs, errors):
    lines = path.read_text().splitlines()
    if lines.count('# implementation-on-master: true') != 1 or any(
            line.startswith('# implementation-on-master:') and line != '# implementation-on-master: true' for line in lines):
        raise ValueError('implementation-on-master flag must be true exactly once')
    rows = csv.DictReader([line for line in lines if line and not line.startswith('#')], delimiter='\t')
    if rows.fieldnames != ['claim_id', 'claim', 'proof_ids']:
        raise ValueError('invalid CLAIMS.tsv header')
    claims, used = [], set()
    for row in rows:
        ident, claim, refs = row['claim_id'], row['claim'], (row['proof_ids'] or '').split()
        if (None in row or not ident or not claim or not refs or any(c['id'] == ident for c in claims)
                or len(refs) != len(set(refs))):
            raise ValueError('invalid or duplicate claim')
        claims.append({'id': ident, 'claim': claim, 'proof_ids': refs})
        errors.extend('unknown proof id: ' + ref for ref in refs if ref not in proofs)
        used.update(refs)
    if not claims:
        errors.append('zero claims')
    errors.extend('unreferenced proof id: ' + ref for ref in sorted(set(proofs) - used))
    return claims


def output_id(label, line, known_proofs=()):
    """Recognize result records independently of their registered pass spelling."""
    tokens = line.split()
    # A known mutation's result is evidence even if its outcome token is
    # malformed or missing. Do not ignore a bad line followed by a good one.
    if label == 'evidence' and tokens:
        for prefix in ('mutant:replay/', 'equivalent:'):
            if prefix + tokens[0] in known_proofs:
                return prefix + tokens[0]
    if label == 'merge':
        if re.match(r'^row \S+(?: |$)', line): return 'case:row-' + line.split()[1]
        if re.match(r'^guard \S+(?: (?!REACHED\b)|$)', line): return 'case:guard/' + line.split()[1]
        m = re.match(r'^core merge (safety|recovery):', line)
        if m: return 'case:' + m[1]
        if re.match(r'^M\d+(?: |$)', line): return 'mutant:' + line.split()[0]
    if label == 'evidence':
        if line.startswith('evidence differential '):
            m = re.match(r'evidence differential (\S+)(?: (.*))?$', line)
            if m and m[1] in ('comparisons:', 'matched', 'refusal-only', 'refusal'):
                return None
            if m: return 'diff:' + m[1]
        m = re.match(r'evidence ([^ :]+)(?: (.*))?$', line)
        if m and m[1] == 'differential':
            return None
        if m: return 'case:' + m[1]
        m = re.match(r'(\S+) (?:KILLED|SURVIVED/ERROR|\|)', line)
        if m: return ('equivalent:' if 'SURVIVED (cannot change an answer)' in line or 'EQUIVALENCE DISPROVED' in line else 'mutant:replay/') + m[1]
    if label in ('codec', 'oracle', 'redelivery'):
        m = re.match(r'codec ([^ :]+)(?: (.*))?$', line)
        if m and m[1] in ('U7a', 'oracle', 'mutants', 'guard', 'redelivery'):
            return None
        if m:
            ident, state = m.groups()
            state = state or ''
            if label == 'oracle': return 'case:golden-bytes' if ident == 'independent_golden_bytes' else 'case:oracle/' + ident
            if label == 'redelivery':
                if ident.startswith('redelivery_case_'): return 'case:redelivery/' + ident[16:]
                return 'diff:redelivery-generated' if ident == 'redelivery_generated' else 'case:redelivery/' + ident
            if state.startswith('KILLED') or ident.startswith(('C0', 'C1', 'G', 'D')): return 'mutant:codec/' + ident
            return 'case:probe/' + ident[6:] if ident.startswith('probe_') else 'case:codec/' + ident
    if label == 'report':
        m = re.match(r'report mutant ([^ :]+)(?::| |$)', line)
        if m: return 'mutant:report/' + m[1]
        m = re.match(r'report (\S+)(?: (.*))?$', line)
        if m and m[1] in ('r3', 'case', 'REDELIVERY', 'harness'):
            return None
        if m:
            ident = m[1]
            return 'case:report/' + (ident[5:] if ident.startswith('case_') else ident[3:] if ident.startswith('r3_') else ident)
    return None


def run_contracts(root, out, provenance):
    definitions, commands = inventories(root)
    reports = []
    for contract, vocabulary in definitions.items():
        errors, claims, runs = [], [], []
        proofs = {ident: {'state': 'NOT-RUN', 'lines': []} for ident in vocabulary}
        try:
            claims = read_claims(root / 'contracts' / contract / 'CLAIMS.tsv', proofs, errors)
            if not errors:
                for label, argv in commands[contract]:
                    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=root)
                    log = contract + '-' + label + '.log'
                    (out / log).write_text(result.stdout)
                    runs.append({'command': argv, 'exit_code': result.returncode, 'log': log,
                                 'log_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
                    if result.returncode:
                        errors.append(f"command failed ({result.returncode}): {' '.join(argv)}")
                    frame = {'merge': 'MERGE', 'evidence': 'EVIDENCE',
                             'codec': 'CODEC', 'report': 'REPORT'}.get(label)
                    inside, begins, ends = False, 0, 0
                    for line in result.stdout.splitlines():
                        if frame and line == frame + ' BASELINE BEGIN':
                            begins += 1
                            inside = True
                            continue
                        if frame and line == frame + ' BASELINE END':
                            ends += 1
                            inside = False
                            continue
                        ident = output_id(label, line, vocabulary)
                        if ident is None: continue
                        if ident not in vocabulary or vocabulary[ident]['runner'] != label:
                            errors.append('unknown proof id in output: ' + ident)
                            continue
                        baseline = (label in ('merge', 'evidence') and ident.startswith('case:') or
                                    label == 'codec' and ident.startswith('case:codec/') or
                                    label == 'report' and ident.startswith('case:report/') and
                                    not ident[12:].isdigit())
                        # Mutant diagnostics cannot supply (or overwrite) the
                        # unmutated C baseline's evidence. This is the same
                        # boundary enforced for wait_time-v1 in core-status.sh.
                        if frame and baseline != inside:
                            if inside:
                                errors.append('unexpected proof in contract baseline: ' + ident)
                            continue
                        proof = proofs[ident]
                        proof['state'] = 'PASS' if not proof['lines'] and re.fullmatch(vocabulary[ident]['pass_pattern'], line) else 'FAIL'
                        proof['lines'].append(line)
                    if frame and (inside or begins != 1 or ends != 1):
                        errors.append('missing, duplicate or unfinished contract baseline: ' + label)
        except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
            errors.append(str(error))
        reports.append({'contract': contract, 'commit': provenance['commit'], 'tree_hash': provenance['tree_hash'],
                        'contract_sha256': contract_hash(root / 'contracts' / contract),
                        'claims': claims, 'proofs': proofs, 'runs': runs, 'errors': errors})
    return reports


def finish(report, out):
    for claim in report['claims']:
        states = [report['proofs'].get(ref, {'state': 'NOT-RUN'})['state'] for ref in claim['proof_ids']]
        claim['state'] = 'FAIL' if 'FAIL' in states else 'NOT-RUN' if 'NOT-RUN' in states else 'PASS'
        print(f"CORE-STATUS {report['contract']}:{claim['id']} {claim['state']}")
    report['state'] = 'FAIL' if report['errors'] or any(c['state'] == 'FAIL' for c in report['claims']) else (
        'NOT-RUN' if not report['claims'] or any(c['state'] == 'NOT-RUN' for c in report['claims']) else 'PASS')
    path = out / (report['contract'] + '.json')
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)
    for error in report['errors']:
        print('CORE-STATUS ERROR ' + report['contract'] + ': ' + error, file=sys.stderr)
    return report['state'] == 'PASS'
