#!/usr/bin/env python3
"""Portable discovery consumer oracle and mutants; no producer qualification.

The frozen table includes all 39 row IDs and verbatim inputs/answers. AOP rows
exercise consumption of the supplied producer facts, not physical BPF operations.
D13b's coverage-a2-v1 UNTRACED verdict is explicitly deferred: that evaluator is
not present in this tree. Do not interpret this host result as all oracle output
assertions or producer/EL8 qualification passing.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DPGWT_TRANSPORT_HOST_TEST', '-Isrc']
SOURCES = ['tests/discovery_admission_host.c', 'src/percpu_transport.c',
           'src/core/merge_adapter.c', 'src/core/merge.c', 'src/coverage.c']
# Each mutation is an executable product change, checked for successful build,
# a reached named oracle row, and the intended assertion. Physical producer
# mutants remain the producer unit's obligation; abstract proof tests do not
# substitute for them.
MUTANTS = [
    ('unknown_admission_fact', 'src/core/pgwt_discovery.h',
     '(f.known & f.truth & required)', '(f.truth & required)',
     'D04a-binding', 'exact merge outputs'),
    ('first_fresh_certified', 'src/core/merge.c',
     '(published->discovery.has_lower_bound || quiet)', 'true',
     'D01', 'exact merge outputs'),
    ('stale_marker_baseline', 'src/core/merge.c',
     '!published->quiet_ready && (!v->record.discovery.marker ||',
     '!published->quiet_ready && (true ||',
     'D01', 'stale marker cannot establish lower bound'),
    ('close_at_enable', 'src/core/merge.c',
     'a->discovery.baseline.e = d->e;',
     'a->discovery.baseline.e = d->e; a->discovery.has_lower_bound = true; a->discovery.lower_bound = d->e;',
     'D07a', 'startup closure commitment'),
    ('ignore_pending_candidate', 'src/merge_capture.c',
     '(!s.has_lower_bound && !s.terminal_released)) return 0;',
     '(!s.has_lower_bound && !s.terminal_released)) continue;',
     'D02e', 'startup closure commitment'),
    ('pre_enable_baseline', 'src/core/merge.c',
     'r->t >= a->discovery.baseline.e;', 'true;',
     'Q09', 'pre-enable record cannot establish lower bound'),
    ('counter_state_witness', 'src/core/merge.c',
     'w->S0 == PGWT_DISC_INACTIVE_SYSCALL && w->S1 == PGWT_DISC_INACTIVE_SYSCALL',
     '(w->S0 == PGWT_DISC_INACTIVE_SYSCALL || w->S0 == PGWT_DISC_STATE_COUNTER_ONLY) && '
     '(w->S1 == PGWT_DISC_INACTIVE_SYSCALL || w->S1 == PGWT_DISC_STATE_COUNTER_ONLY)',
     'Q04', 'exact merge outputs'),
    ('schedstat_witness', 'src/core/merge.c',
     'w->S0 == PGWT_DISC_INACTIVE_SYSCALL && w->S1 == PGWT_DISC_INACTIVE_SYSCALL',
     '(w->S0 == PGWT_DISC_INACTIVE_SYSCALL || w->S0 == PGWT_DISC_SCHEDSTAT_ONLY) && '
     '(w->S1 == PGWT_DISC_INACTIVE_SYSCALL || w->S1 == PGWT_DISC_SCHEDSTAT_ONLY)',
     'Q04b', 'exact merge outputs'),
    ('denied_S1', 'src/core/merge.c',
     'w->S1 == PGWT_DISC_INACTIVE_SYSCALL',
     '(w->S1 == PGWT_DISC_INACTIVE_SYSCALL || w->S1 == PGWT_DISC_DENIED)',
     'Q05', 'exact merge outputs'),
    ('equal_completeness', 'src/core/merge.c',
     'W > a->discovery.baseline.t;', 'W >= a->discovery.baseline.t;',
     'D02a', 'unresolved lifetime count'),
    ('completion_at_C', 'src/merge_capture.c',
     '.at = close + 1,', '.at = close,',
     'D01', 'reader excludes completion exactly at C'),
    ('revoke_at_detection', 'src/merge_capture.c',
     '.at = c->coverage_boot_start, .detected = now}', '.at = now, .detected = now}',
     'D10', 'revoke from boot start'),
    ('auto_rearm', 'src/core/merge.c',
     '/* Controls do not recover a suspended epoch or replace its evidence. */',
     'm->report.status = PGWT_MERGE_OK; /* wrong: discovery recovers */',
     'D13a', 'discovery cannot auto-REARM'),
    ('changed_voluntary_count', 'src/core/merge.c',
     'w->v0 == w->v1 &&', 'true &&',
     'Q02', 'exact merge outputs'),
    ('running_endpoint', 'src/core/merge.c',
     'w->S1 == PGWT_DISC_INACTIVE_SYSCALL',
     '(w->S1 == PGWT_DISC_INACTIVE_SYSCALL || w->S1 == PGWT_DISC_RUNNING)',
     'Q03', 'exact merge outputs'),
    ('multiple_threads', 'src/core/merge.c',
     'w->threads0 == 1 && w->threads1 == 1 &&', 'true &&',
     'Q06', 'exact merge outputs'),
    ('zero_cpu_invalid', 'src/core/merge.c',
     'w->cpu_base_valid &&', 'w->cpu_base_valid && w->cpu_base != 0 &&',
     'Q07', 'exact merge outputs'),
    ('reject_all_quiet', 'src/core/merge.c',
     'bool valid = quiet_witness_valid(w);', 'bool valid = quiet_witness_valid(w) && false;',
     'Q01', 'exact merge outputs'),
    ('skip_address_match', 'src/core/merge.c',
     'w->armed_address == w->resolved_address &&', 'true &&',
     'Q08', 'address mismatch rejects quiet candidate'),
    ('publish_failed_lower_bound', 'src/core/merge.c',
     'return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);\n        /* DECISION 25c:',
     '{ /* incorrectly continue after failed publication */ }\n        /* DECISION 25c:',
     'D13d', 'exact merge outputs'),
]


def run(command, tree, check=True):
    result = subprocess.run(command, cwd=tree, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError(f'{shlex.join(map(str, command))}\n{result.stdout}{result.stderr}')
    return result


def build(tree):
    binary = tree / 'discovery-host'
    run(shlex.split(os.environ.get('CC', 'cc')) + FLAGS + shlex.split(os.environ.get('CFLAGS', '')) +
        SOURCES + ['-o', str(binary)], tree)
    return binary


def check_oracle_inventory():
    def markdown(path):
        return dict(re.findall(r'^\| ([DQ][^ |]+) \| (.*?) \|$', path.read_text(), re.M))
    rows = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/ROWS.md')
    answers = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/EXPECTED.md')
    table = []
    for line in (ROOT / 'tests/discovery_oracle_rows.h').read_text().splitlines():
        if line.startswith('    {'):
            table.append(json.loads('[' + line.strip()[1:-2] + ']'))
    ids = [row[0] for row in table]
    assert len(ids) == len(set(ids)) == 39
    assert set(ids) == set(rows) == set(answers), 'oracle row IDs drifted'
    assert all(inputs == rows[key] and answer == answers[key] for key, inputs, answer in table)
    print('discovery frozen table: 39/39 IDs and verbatim inputs/answers match', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutants', action='store_true')
    args = parser.parse_args()
    check_oracle_inventory()
    with tempfile.TemporaryDirectory(prefix='pgwt-discovery-') as temporary:
        tree = Path(temporary)
        shutil.copytree(ROOT / 'src', tree / 'src')
        (tree / 'tests').mkdir()
        for name in ['discovery_admission_host.c', 'discovery_oracle_rows.h', 'test_percpu_transport.c',
                     'test_core_merge_adapter.c', 'test_core_merge_contract.c', 'merge_adapter_goldens.h']:
            shutil.copy2(ROOT / 'tests' / name, tree / 'tests' / name)
        print(run([str(build(tree))], tree).stdout, end='', flush=True)
        if args.mutants:
            for name, path, before, after, row, assertion in MUTANTS:
                file = tree / path
                source = file.read_text()
                assert source.count(before) == 1, (name, source.count(before))
                try:
                    file.write_text(source.replace(before, after))
                    binary = build(tree)
                    result = run([str(binary), row], tree, False)
                    assert f'REACHED discovery {row}' in result.stdout, (name, result.stdout)
                    assert result.returncode == 1 and f'ASSERT {row}: {assertion} (line ' in result.stderr, (
                        name, result.returncode, result.stdout, result.stderr)
                    print(f'MUTANT {name}: RED on {row} ({assertion})', flush=True)
                finally:
                    file.write_text(source)
    print('PARTIAL: physical AOP assertions, numerical interval assertions, and D13b A2 verdict remain outside this consumer harness', flush=True)


if __name__ == '__main__':
    main()
