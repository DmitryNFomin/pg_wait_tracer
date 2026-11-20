#!/usr/bin/env python3
"""Discovery full consumer outputs, numerical authority and population mutants.

Physical AOP facts excluded from the consumer unit are enumerated in
discovery_oracle_assertions.txt. They are not claimed as producer qualification.
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
           'src/core/merge_adapter.c', 'src/core/merge.c', 'src/coverage.c', 'src/core/wait_metrics.c']
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
    ('discovery_as_a2_birth', 'src/merge_capture.c',
     'if (pgwt_a2_observed_birth(&birth, c->coverage_boot_start) &&\n'
     '            from <= birth.fork_ns && birth.fork_ns < to) ++n;',
     'if ((birth.discovery && from <= c->coverage_boot_start && c->coverage_boot_start < to) ||\n'
     '            (pgwt_a2_observed_birth(&birth, c->coverage_boot_start) &&\n'
     '             from <= birth.fork_ns && birth.fork_ns < to)) ++n;',
     'D13b', 'discovery contributes zero to N_k'),
    ('discovery_as_a2_omission', 'src/merge_capture.c',
     'return pgwt_a2_omission_population(&l, c->coverage_boot_start, u);',
     'if (l.discovery) return PGWT_A2_POPULATION_OK;\n'
     '    return pgwt_a2_omission_population(&l, c->coverage_boot_start, u);',
     'D13b', 'A2 result Unavailable/untraced_invalid for discovery omission'),
    ('ignore_census_fence', 'src/merge_capture.c',
     'uint64_t close = c->census_fence;', 'uint64_t close = c->coverage_boot_start;',
     'D07b', 'startup closure commitment'),
    ('equal_census_completeness', 'src/merge_capture.c',
     'report.frontier < close || close == UINT64_MAX',
     'report.frontier + 1 < close || close == UINT64_MAX',
     'D07b', 'census completeness must be strictly beyond closure'),
    ('terminal_on_receipt', 'src/core/merge.c',
     'a->terminal_at = d->terminal_time;',
     'a->terminal_at = d->terminal_time; a->discovery.terminal_released = true; '
     'a->discovery.terminal_time = d->terminal_time;',
     'D08a', 'unresolved lifetime count'),
    ('esrch_terminal_proof', 'src/core/merge.c',
     'if (d->terminal_binding_proved) {', 'if (true) {',
     'D08b', 'ESRCH cannot authorize matching EXIT'),
    ('queued_cookie_current_lifetime', 'src/core/merge_adapter.c',
     'record.g = e->cookie_present && e->cookie ? e->cookie : 0;',
     'record.g = t->lifetime_count ? t->lifetimes[t->lifetime_count - 1].g : 0;',
     'D09', 'exact merge outputs'),
    ('cpu_replaced_by_wall', 'src/window_clip.h',
     'uint64_t share = fb - fa, wall = b - a;',
     'uint64_t share = fb - fa, wall = b - a; share = wall;',
     'Q07', 'measured CPU is one, not zero or elapsed time'),
    ('zero_wait_infers_on_cpu', 'src/core/merge.c',
     'a->discovery.baseline = *w;',
     'a->discovery.baseline = *w; if (!w->wait_word) a->discovery.baseline.on_cpu_ts = w->t;',
     'Q07', 'valid zero CPU base and off-CPU seed'),
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
    expected_text = (ROOT / 'tests/discovery_oracle_expected.h').read_text()
    structured_rows = re.findall(r'^    \{"([DQ][^"]+)", "([^"]*)",', expected_text, re.M)
    structured = dict(structured_rows)
    assert len(structured_rows) == len(structured) == len(rows), 'duplicate or missing structured row'
    assert set(structured) == set(rows), 'structured expected row IDs drifted'
    for key, answer in answers.items():
        sequences = [code for code in re.findall(r'`([^`]+)`', answer)
                     if re.match(r'(?:D\d+\[|F\d+\b|VOID\d+)', code)]
        sequence = sequences[0] if sequences else ''
        if key == 'D10':
            sequence = structured['D01'] + ' F30[C0]'
        else:
            sequence = ' '.join(sequence.replace('→', ' ').split())
            sequence = re.sub(r'\bF(\d+)\b', r'F\1[C0]', sequence)
        assert structured[key] == sequence, (key, structured[key], sequence)
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
        for name in ['discovery_admission_host.c', 'discovery_oracle_rows.h', 'discovery_oracle_expected.h', 'test_percpu_transport.c',
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
    print('Physical producer assertions not encoded: see tests/discovery_oracle_assertions.txt', flush=True)


if __name__ == '__main__':
    main()
