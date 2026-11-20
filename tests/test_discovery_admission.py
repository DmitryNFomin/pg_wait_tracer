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
# Mutations change executed product logic (or the named numerical consumer
# adapter in this host harness), checked for successful build,
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
    ('never_C_plus_one', 'src/merge_capture.c',
     '.at = close + (increment ? 1 : 0),', '.at = close,',
     'D01', 'reader availability at semantic C'),
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
     'report.frontier < close) return 0;',
     'report.frontier + 1 < close) return 0;',
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

    # Persisted endpoints and permanent abandonment, Decisions 41-42/45.
    ('always_C_plus_one', 'src/merge_capture.c',
     '.at = close + (increment ? 1 : 0),',
     '.at = close + 1,',
     'Q01', 'persisted startup endpoint and publication time'),
    ('ignore_earlier_attempt_endpoint', 'src/core/merge.c',
     '(discovery_transition || discard) && !out.prior_interval_certified',
     'final_record && !out.prior_interval_certified',
     'D14c', 'persisted startup endpoint and publication time'),
    ('ignore_discard_endpoint', 'src/core/merge.c',
     '(discovery_transition || discard) && !out.prior_interval_certified',
     'discovery_transition && !out.prior_interval_certified',
     'A03', 'persisted startup endpoint and publication time'),
    ('increment_for_earlier_uncertified_stamp', 'src/merge_capture.c',
     's.uncertified_through == close',
     's.uncertified_through <= close',
     'D14b', 'persisted startup endpoint and publication time'),
    ('EXIT_is_numerical_endpoint', 'src/merge_capture.c',
     'if (s.has_uncertified && s.uncertified_through == close) increment = true;',
     'if ((s.has_uncertified && s.uncertified_through == close) || (s.terminal_released && s.terminal_time == close)) increment = true;',
     'A20', 'persisted startup endpoint and publication time'),
    ('nonzero_reserved_endpoint', 'src/coverage_record.h',
     'pgwt_cov_put_u64(out + 16, r->b);',
     'pgwt_cov_put_u64(out + 16, r->kind == PGWT_COVK_GAP_CLOSE ? 1 : r->b);',
     'D01', 'GAP_CLOSE reserved fields stay zero'),
    ('certified_completion_at_C_dropped', 'src/core/wait_metrics.c',
     'q->from <= w.end && w.end < q->to',
     'q->from < w.end && w.end < q->to',
     'D14b', 'X duration and completed count'),
    ('numerical_adapter_uses_lower_bound', 'tests/discovery_admission_host.c',
     'bool available=base_verdict(h,from,to)==PGWT_COV_COMPLETE;',
     'pgwt_discovery_state state={0}; (void)pgwt_merge_discovery_state(h->capture->core,30,DG,&state);\n    bool available=state.has_lower_bound && DB+from>=state.lower_bound;',
     'D01', 'reader refuses window containing uncertified endpoint'),
    ('unknown_completed_count_becomes_zero', 'src/core/wait_metrics.c',
     'completed->value.present = !completion_gap;',
     'completed->value.present = !completion_gap || count == 0;',
     'D01', 'unknown completed count at C is unavailable'),
    ('abandon_supplies_lower_bound', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'a->discovery.abandoned = true; a->discovery.has_lower_bound = true; a->discovery.lower_bound = obs.d;',
     'A01', 'full gap state and census'),
    ('abandon_keeps_missing_ADMIT_dependency', 'src/core/merge.c',
     '!a->identity_proved &&\n            !generation->discovery.abandoned',
     '!a->identity_proved',
     'A01', 'complete output sequence with counter labels'),
    ('publish_abandoned_staged_TRANS', 'src/core/merge.c',
     'bool discard = published && published->discovery.abandoned &&',
     'bool discard = false && published && published->discovery.abandoned &&',
     'A01', 'complete output sequence with counter labels'),
    ('publish_abandoned_future_TRANS', 'src/core/merge.c',
     'bool discard = published && published->discovery.abandoned &&',
     'bool discard = !m->report.has_frontier && published && published->discovery.abandoned &&',
     'A02', 'complete output sequence with counter labels'),
    ('publish_abandoned_TEXT', 'src/core/merge.c',
     '(v->text || transition);',
     'transition;',
     'A19', 'exclusive abandoned TEXT is never published'),
    ('publish_abandoned_MARKER', 'src/core/merge.c',
     '(v->text || transition);',
     '(v->text || (transition && !v->record.discovery.marker));',
     'A19', 'complete output sequence with counter labels'),
    ('omit_discard_diagnostic', 'src/core/merge.c',
     '++m->report.discovery_abandoned_records;',
     'm->report.discovery_abandoned_records += 0;',
     'A01', 'cumulative consumed abandonment count'),
    ('double_discard_diagnostic', 'src/core/merge.c',
     '++m->report.discovery_abandoned_records;',
     'm->report.discovery_abandoned_records += 2;',
     'A01', 'cumulative consumed abandonment count'),
    ('reset_discard_diagnostic_on_REARM', 'src/core/merge.c',
     '++m->report.epoch;',
     '++m->report.epoch; m->report.discovery_abandoned_records = 0;',
     'A12', 'cumulative consumed abandonment count'),
    ('count_staged_records_at_abandon', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'a->discovery.abandoned = true; m->report.discovery_abandoned_records += m->count;',
     'A13', 'cumulative consumed abandonment count'),
    ('count_rejected_record_as_discard', 'src/core/merge.c',
     'static pgwt_merge_status late_fault(pgwt_merge *m, pgwt_merge_reason reason,\n                                    const pgwt_merge_input *in)\n{',
     'static pgwt_merge_status late_fault(pgwt_merge *m, pgwt_merge_reason reason,\n                                    const pgwt_merge_input *in)\n{\n    if (in->record.g >= PGWT_DISCOVERY_COOKIE_MIN) ++m->report.discovery_abandoned_records;',
     'A11', 'cumulative consumed abandonment count'),
    ('discard_lifecycle_EXIT', 'src/core/merge.c',
     '(v->text || transition);',
     '(v->text || transition || v->record.rank == PGWT_MERGE_EXIT);',
     'A03', 'complete output sequence with counter labels'),
    ('terminal_discharge_on_receipt_abandoned', 'src/core/merge.c',
     'a->terminal_at = d->terminal_time;',
     'a->terminal_at = d->terminal_time; a->discovery.terminal_released = true; a->discovery.terminal_time = d->terminal_time;',
     'A03', 'unresolved lifetime count'),
    ('terminal_release_at_equal_W', 'src/core/merge.c',
     'm->queue[eligible].record.t < W))',
     'm->queue[eligible].record.t <= W))',
     'A03', 'exact merge outputs'),
    ('abandon_after_ADMIT', 'src/core/merge.c',
     '(a && a->discovery.admitted)))',
     '(false && a && a->discovery.admitted)))',
     'A04', 'complete output sequence with counter labels'),
    ('ADMIT_after_abandon', 'src/core/merge.c',
     'in->kind != PGWT_MERGE_TERMINAL_DISCOVERY) {',
     'in->kind != PGWT_MERGE_TERMINAL_DISCOVERY && in->kind != PGWT_MERGE_ADMIT_DISCOVERY) {',
     'A05', 'complete output sequence with counter labels'),
    ('CONFIRM_after_abandon', 'src/core/merge.c',
     'in->kind != PGWT_MERGE_TERMINAL_DISCOVERY) {',
     'in->kind != PGWT_MERGE_TERMINAL_DISCOVERY && in->kind != PGWT_MERGE_CONFIRM_DISCOVERY) {',
     'A10', 'complete output sequence with counter labels'),
    ('abandon_uninventoried_identity', 'src/core/merge.c',
     '!pgwt_discovery_has(d->proof, PGWT_DISC_INVENTORIED |\n                PGWT_DISC_STARTUP_OPEN | PGWT_DISC_IDENTITY_VISIBLE)',
     'false',
     'A06', 'complete output sequence with counter labels'),
    ('first_abandon_after_closure', 'src/core/merge.c',
     '!pgwt_discovery_has(d->proof, PGWT_DISC_INVENTORIED |\n                PGWT_DISC_STARTUP_OPEN | PGWT_DISC_IDENTITY_VISIBLE)',
     'false',
     'A20', 'complete output sequence with counter labels'),
    ('reject_duplicate_after_closure', 'src/core/merge.c',
     'if (!(a && a->discovery.abandoned) &&',
     'if (true &&',
     'A09', 'complete output sequence with counter labels'),
    ('reapply_duplicate_abandon', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'if (a->discovery.abandoned) ++m->report.discovery_abandoned_records; a->discovery.abandoned = true;',
     'A09', 'cumulative consumed abandonment count'),
    ('forgive_counter_change_after_abandon', 'src/core/merge.c',
     '/* Observation checkpoint is independent of the valid epoch baseline. */',
     '/* Wrong: abandonment forgives a changed vector. */\n    for (size_t i = 0; i < m->attachment_count; ++i) if (m->attachments[i].discovery.abandoned) { m->baseline = m->observed = obs.counters; return m->report.status; }',
     'A08', 'complete output sequence with counter labels'),
    ('manufacture_storage_loss_for_abandon', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'a->discovery.abandoned = true; ++m->baseline.value[14];',
     'A01', 'complete output sequence with counter labels'),
    ('skip_abandoned_arrival_boundary', 'src/core/merge.c',
     'if (r->t < src->accepted_boundary)',
     'if (r->g < PGWT_DISCOVERY_COOKIE_MIN && r->t < src->accepted_boundary)',
     'A11', 'complete output sequence with counter labels'),
    ('suppress_frontier_invalidation', 'src/core/merge.c',
     'm->report.epoch_invalid = true;',
     'm->report.epoch_invalid = false;',
     'A11', 'oracle does not invalidate epochs or require process exit'),
    ('forget_abandonment_on_REARM', 'src/core/merge.c',
     'm->attachments[i].creation = 0;',
     'm->attachments[i].creation = 0; m->attachments[i].discovery.abandoned = false;',
     'A12', 'complete output sequence with counter labels'),
    ('abandon_supplies_completeness', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'a->discovery.abandoned = true; for (size_t s = 0; s < m->config.source_count; ++s) m->sources[s].proof.cutoff = obs.d;',
     'A13', 'complete output sequence with counter labels'),
    ('release_without_completed_failure_accounting', 'src/core/merge_adapter.c',
     'c->known = snapshot && snapshot->valid;',
     'c->known = snapshot != NULL;',
     'A14', 'complete output sequence with counter labels'),
    ('terminal_commit_after_rejected_EXIT', 'src/core/merge.c',
     '} else if (!m->config.sink(m->config.sink_context, &out, &obs))\n            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);',
     '} else if (!m->config.sink(m->config.sink_context, &out, &obs)) {\n            if (published && v->record.rank == PGWT_MERGE_EXIT) { published->discovery.terminal_released = true; published->discovery.terminal_time = v->record.t; }\n            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);\n        }',
     'A15', 'full gap state and census'),
    ('abandon_repairs_unknown_attribution', None,
     [('src/core/merge.c', 'if (!r->identity_known)\n                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);', 'if (!r->identity_known && r->g < PGWT_DISCOVERY_COOKIE_MIN)\n                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);'), ('src/core/merge.c', 'if (!r->identity_known || !generation)', 'if ((!r->identity_known && !generation->discovery.abandoned) || !generation)')],
     None,
     'A16', 'uncertain record attribution rejected before discard'),
    ('abandonment_by_PID', 'src/core/merge.c',
     'bool discard = published && published->discovery.abandoned &&',
     'bool abandoned_pid = false; if (published) for (size_t j = 0; j < m->attachment_count; ++j) if (m->attachments[j].proof.pid == published->proof.pid && m->attachments[j].discovery.abandoned) abandoned_pid = true;\n        bool discard = published && abandoned_pid &&',
     'A17', 'complete output sequence with counter labels'),
    ('abandon_auto_REARM', 'src/core/merge.c',
     'a->discovery.abandoned = true;',
     'a->discovery.abandoned = true; m->report.status = PGWT_MERGE_OK;',
     'A18', 'complete output sequence with counter labels'),
    ('skip_abandoned_metadata_dependency', 'src/core/merge.c',
     'if (r->dependency == PGWT_MERGE_WAIT &&',
     'if (!generation->discovery.abandoned && r->dependency == PGWT_MERGE_WAIT &&',
     'A19', 'abandonment cannot bypass missing metadata dependency'),
    ('forget_admission_on_REARM', 'src/core/merge.c',
     'm->attachments[i].creation = 0;',
     'm->attachments[i].creation = 0; m->attachments[i].discovery.admitted = false;',
     'A04', 'admission history forbids abandonment across REARM'),
    ('abandon_reserved_uninstalled_identity', 'src/core/merge.c',
     'PGWT_DISC_STARTUP_OPEN | PGWT_DISC_IDENTITY_VISIBLE) ||',
     'PGWT_DISC_STARTUP_OPEN) ||',
     'A06', 'reserved cookie alone cannot authorize abandonment'),
    ('terminal_ignores_staged_suffix', 'src/core/merge.c',
     'v->record.t > a->terminal_at)',
     'false)',
     'A03', 'terminal proof reconciles staged records beyond EXIT before discharge'),
    ('forget_published_EXIT_on_counter_fault', 'src/core/merge.c',
     'if (published && !v->text && v->record.rank == PGWT_MERGE_EXIT) {',
     'if (published && !v->text && v->record.rank == PGWT_MERGE_EXIT && same_counters(obs.counters, m->baseline)) {',
     'A03', 'successful EXIT survives counter fault for later terminal reconciliation'),
    ('omit_abandonment_from_wire_identity', 'src/merge_capture.c',
     'controls + rejected +\n        s.discovery_abandoned_records;',
     'controls + rejected;',
     'A01', 'wire identity accounts for consumed abandonment separately'),
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
        return dict(re.findall(r'^\| ([DQA][^ |]+) \| (.*?) \|$', path.read_text(), re.M))
    rows = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/ROWS.md')
    answers = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/EXPECTED.md')
    table = []
    for line in (ROOT / 'tests/discovery_oracle_rows.h').read_text().splitlines():
        if line.startswith('    {'):
            table.append(json.loads('[' + line.strip()[1:-2] + ']'))
    ids = [row[0] for row in table]
    assert len(ids) == len(set(ids)) == 62
    assert set(ids) == set(rows) == set(answers), 'oracle row IDs drifted'
    assert all(inputs == rows[key] and answer == answers[key] for key, inputs, answer in table)
    expected_text = (ROOT / 'tests/discovery_oracle_expected.h').read_text()
    structured_rows = re.findall(r'^    \{"([DQA][^"]+)", "([^"]*)",', expected_text, re.M)
    structured = dict(structured_rows)
    assert len(structured_rows) == len(structured) == len(rows), 'duplicate or missing structured row'
    assert set(structured) == set(rows), 'structured expected row IDs drifted'
    states = dict((key, (int(persisted), int(pending), int(abandoned)))
        for key, persisted, pending, abandoned in re.findall(
            r'^    \{"([DQA][^"]+)", "[^"]*", -?\d+, (-?\d+), -?\d+, (\d+), \'[FQN]\', (?:true|false), (\d+)\},$',
            expected_text, re.M))
    assert set(states) == set(rows), 'structured state row IDs drifted'
    for key, answer in answers.items():
        codes = re.findall(r'`([^`]+)`', answer)
        sequence = next((code for code in codes if '→' in code or re.match(r'VOID\d+', code)), None)
        if sequence is None:
            sequence = next((code for code in codes if re.fullmatch(r'F\d+', code)), '')
        if key == 'D10':
            sequence = 'D12[U] → D15[T] → F15 → F30'
        sequence = ' '.join(sequence.replace('→', ' ').split())
        sequence = re.sub(r'\bF(\d+)\b', r'F\1[C0]', sequence)
        assert structured[key] == sequence, (key, structured[key], sequence)
        gap_ends = re.findall(r'G\((open|\d+)\)', answer)
        persisted = int(gap_ends[-1]) if gap_ends and gap_ends[-1] != 'open' else -1
        pending = re.findall(r'P=(\d+)', answer)
        abandoned = re.findall(r'abandoned=(\d+)', answer)
        assert states[key] == (persisted, int(pending[-1]) if pending else 1,
                               int(abandoned[-1]) if abandoned else 0), (key, states[key])
    print('discovery frozen table: 62/62 IDs and verbatim inputs/answers match', flush=True)


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
                patches = [(path, before, after)] if isinstance(before, str) else before
                originals = {}
                try:
                    for relative, old, new in patches:
                        file = tree / relative
                        source = file.read_text()
                        assert source.count(old) == 1, (name, relative, source.count(old))
                        originals.setdefault(file, source)
                        file.write_text(source.replace(old, new))
                    binary = build(tree)
                    result = run([str(binary), row], tree, False)
                    assert f'REACHED discovery {row}' in result.stdout, (name, result.stdout)
                    assert result.returncode == 1 and f'ASSERT {row}: {assertion} (line ' in result.stderr, (
                        name, result.returncode, result.stdout, result.stderr)
                    assert not re.search(r'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:',
                                         result.stdout + result.stderr), (name, result.stderr)
                    print(f'MUTANT {name}: RED on {row} ({assertion})', flush=True)
                finally:
                    for file, source in originals.items():
                        file.write_text(source)
            print(f'discovery mutants: {len(MUTANTS)}/{len(MUTANTS)} RED; built, reached named row, intended assertion', flush=True)
    print('Physical producer assertions not encoded: see tests/discovery_oracle_assertions.txt', flush=True)


if __name__ == '__main__':
    main()
