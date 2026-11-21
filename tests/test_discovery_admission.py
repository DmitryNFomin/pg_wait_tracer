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
           'src/core/merge_adapter.c', 'src/coverage.c', 'src/core/wait_metrics.c']
# Mutations change executed product logic (the pre-existing numerical reader
# seam is explicitly named), checked for successful build,
# a reached named oracle row, and the intended assertion. Physical producer
# mutants remain the producer unit's obligation; abstract proof tests do not
# substitute for them.
MUTANTS = [
    ('unknown_admission_fact', 'src/core/pgwt_discovery.h',
     '(f.known & f.truth & required)', '(f.truth & required)',
     'D04a-binding', 'exact merge outputs'),
    ('first_fresh_certified', 'src/core/merge.c',
     '(planned->discovery.has_lower_bound || quiet)', 'true',
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
     '*endpoint = close + (increment ? 1 : 0);', '*endpoint = close + (increment ? 0 : 0);',
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
    ('reject_all_quiet', 'src/core/merge.c', 'bool valid = no_word || quiet_witness_valid(w);',
 'bool valid = no_word || (quiet_witness_valid(w) && false);', 'Q01', 'exact merge outputs'),
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
     '*endpoint = close + (increment ? 1 : 0);',
     '*endpoint = close + 1;',
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
    ('omit_abandonment_from_wire_identity', 'src/merge_capture.c',
     'controls + rejected +\n        s.discovery_abandoned_records;',
     'controls + rejected;',
     'A01', 'wire identity accounts for consumed abandonment separately'),
    ('a_abandoned_after_terminal_arrival', 'src/core/merge.c',
     '    if (a->discovery.abandoned && a->terminal_proved && r->t > a->terminal_at)\n        return false;\n',
     '',
     'G_after_terminal_arrival', 'guard complete expected output'),
    ('b_terminal_uncertified_check', 'src/core/merge.c',
     '            if (a->discovery.has_uncertified &&\n                a->discovery.uncertified_through > a->terminal_at)\n                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n',
     '',
     'G_consumed_past_terminal', 'guard complete expected output'),
    ('c_confirm_staged_reconcile', 'src/core/merge.c',
     '              r->t >= w->e) && r->t > w->t)\n            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);',
     '              r->t >= w->e) && r->t > w->t)\n            continue;',
     'G_confirm_staged_attempt', 'guard complete expected output'),
    ('d_post_batch_reconcile', 'src/core/merge.c',
     '    if (reconcile_discovery) {',
     '    if (false && reconcile_discovery) {',
     'G_batch_staged_attempt', 'guard complete expected output'),
    ('e_ambiguous_terminal_match', 'src/merge_capture.c',
     '            if (cookie && cookie != v->cookie)\n                return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, *s);\n',
     '',
     'G_ambiguous_terminal_owner', 'guard complete expected output'),
    ('i_abandoned_rank', 'src/core/merge.c',
     '            if (r->rank != PGWT_MERGE_TRANS && r->rank != PGWT_MERGE_EXIT)\n                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);',
     '            ;',
     'G_abandoned_fork', 'guard complete expected output'),
    ('j_exclusive_text_owner', 'src/core/merge_adapter.c',
     'if (!owner || !owner->identity_bound || record.g < PGWT_DISCOVERY_COOKIE_MIN)',
     'if ((void)owner, record.g < PGWT_DISCOVERY_COOKIE_MIN)',
     'G_exclusive_text_binding', 'guard complete expected output'),
    ('k_cookieless_exit_uncovered', 'src/merge_capture.c',
     '                uncovered = false;\n                break;',
     '                break;',
     'T2', 'cookie-less EXIT and frontier wait only for disposition'),
    ('l_terminal_time_match', 'src/merge_capture.c', ': v->terminal_at == at);', ': ((void)at, true));',
 'G_cookie_less_time_match', 'cookie-less EXIT must match terminal time before binding'),
    ('m_exit_time_mismatch', 'src/core/merge.c',
     '                if (a->exit_record.t != a->terminal_at)\n                    return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n',
     '',
     'G_exit_time_conflict', 'guard complete expected output'),
    ('o_capture_terminal_conflict', 'src/merge_capture.c',
 '                    if (v->terminal_proved && v->terminal_at != a->terminal_time)\n'
 '                        return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);\n',
 '', 'G_terminal_control_conflict', 'guard complete expected output'),
    ('r_overflow_close', 'src/merge_capture.c',
     '    if (increment && close == UINT64_MAX) return false;\n',
     '',
     'G_endpoint_overflow', 'overflow supplies no persisted endpoint'),
    ('i_abandoned_init_rank', 'src/core/merge.c',
     '            if (r->rank != PGWT_MERGE_TRANS && r->rank != PGWT_MERGE_EXIT)\n                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);',
     '            ;',
     'G_abandoned_init', 'guard complete expected output'),
    ('release_held_EXIT_on_boundary', 'src/merge_capture.c',
     '            *hold = true;\n            return rc;',
     '            *hold = false;\n            return rc;',
     'T2', 'cookie-less EXIT and frontier wait only for disposition'),
    ('unresolved_through_late_unlisted', 'src/merge_capture.c',
     '            ++c->uncovered_records;',
     '            ++c->uncovered_records; c->census_complete = false; c->inventory[c->inventory_count++] = (struct discovery_inventory){.pid = pid};',
     'T1', 'unresolved disposition preserves inventory'),
    ('matched_after_unresolved', 'src/merge_capture.c', 'if (v->unresolved != unresolved)',
 'if (!v->unresolved && unresolved)', 'T3', 'complete output sequence with counter labels'),
    ('unresolved_after_matched', 'src/merge_capture.c', 'if (v->unresolved != unresolved)',
 'if (v->unresolved && !unresolved)', 'G_matched_then_unresolved',
 'guard complete expected output'),
    ('reapply_unresolved_duplicate', 'src/merge_capture.c',
 'if (v->cookie == a->cookie) duplicate = true;',
 'if (v->cookie == a->cookie) { duplicate = true; if (unresolved) ++c->uncovered_records; }',
 'G_unresolved_duplicate', 'duplicate unresolved disposition never recounts EXIT'),
    ('discharge_VOIDed_EXIT_suffix', 'src/core/merge.c',
     'planned->exit_release_committed = true;',
     'planned->exit_release_committed = true; published->exit_release_committed = true;',
     'V1', 'full gap state and census'),
    ('discharge_depends_on_TERM_order', 'src/core/merge.c',
     'planned->exit_release_committed = true;',
     'planned->exit_release_committed = true; published->exit_release_committed = true;',
     'V2', 'full gap state and census'),
    ('discharge_before_later_rejection', 'src/core/merge.c',
     'planned->discovery.terminal_released = true;',
     'published->discovery.terminal_released = true; planned->discovery.terminal_released = true;',
     'V3', 'VOIDed release attempt commits no terminal discharge'),
    ('discharge_before_FRONTIER_rejection', 'src/core/merge.c',
     'planned->discovery.terminal_released = true;',
     'published->discovery.terminal_released = true; planned->discovery.terminal_released = true;',
     'V4', 'VOIDed release attempt commits no terminal discharge'),
    ('lower_bound_before_later_rejection', 'src/core/merge.c',
     'planned->discovery.has_lower_bound = true;',
     'published->discovery.has_lower_bound = true; planned->discovery.has_lower_bound = true;',
     'bound_rejected_later_record', 'VOIDed release commits no fresh lower bound'),
    ('lower_bound_before_FRONTIER_rejection', 'src/core/merge.c',
     'planned->discovery.has_lower_bound = true;',
     'published->discovery.has_lower_bound = true; planned->discovery.has_lower_bound = true;',
     'bound_rejected_frontier', 'VOIDed release commits no fresh lower bound'),
    ('forget_prearrival_disposition', 'src/merge_capture.c',
 '.unresolved=unresolved, .names_exit=a->names_exit,',
 '.unresolved=unresolved && c->retained_count != 0, .names_exit=a->names_exit,', 'T4',
 'unresolved EXIT counted exactly once as uncovered'),
    ('finish_without_disposition', 'src/merge_capture.c',
     '    if (missing_disposition || pgwt_capture_waiting(c))',
     '    if (false)',
     'T2', 'complete output sequence with counter labels'),
    ('discharge_before_frontier_checkpoint', 'src/core/merge.c',
     '        /* Published frontier counter checkpoint. */',
     '        for (size_t i = 0; i < m->touched_count; ++i) { m->attachments[m->touched[i]].discovery = m->prospective[i].discovery; m->attachments[m->touched[i]].exit_release_committed = m->prospective[i].exit_release_committed; }\n        /* Published frontier counter checkpoint. */',
     'G_discharge_frontier_checkpoint', 'VOIDed release attempt commits no terminal discharge'),
    ('forget_missing_disposition_on_suspension', 'src/merge_capture.c',
     '    if (missing_disposition || pgwt_capture_waiting(c))',
     '    if ((void)missing_disposition, pgwt_capture_waiting(c))',
     'G_missing_disposition_after_fault', 'guard complete expected output'),
    ('double_count_unresolved_after_fault', 'src/merge_capture.c',
     '                --c->dropped_records;',
     '                (void)c->dropped_records;',
     'G_unresolved_after_fault', 'unresolved EXIT counted once after fault'),
    ('forget_unresolved_receipt_drop', 'src/merge_capture.c',
     'n->receipt_dropped = !token &&',
     'n->receipt_dropped = false && !token &&',
     'G_unresolved_received_suspended', 'unresolved EXIT counted once after fault'),
    ('remove_reused_arrival_owner_check', 'src/merge_capture.c',
     'arrival->source == e->source &&',
     'true &&',
     'G_unresolved_reused_token', 'unresolved EXIT preserves reused arrival token'),
    ('underflow_unresolved_drop_count', 'src/merge_capture.c',
     '                if (!c->dropped_records)\n                    return pgwt_capture_fault(c, PGWT_MERGE_QUEUE_OWNERSHIP, d, *s);\n',
     '',
     'G_unresolved_bad_drop_count', 'guard complete expected output'),
    ('release_raw_EXIT_during_suspension', 'src/merge_capture.c',
     'bool terminal_hold = c->config.discovery_enabled &&',
     'bool terminal_hold = false && c->config.discovery_enabled &&',
     'G_missing_disposition_during_suspension', 'guard complete expected output'),
    ('remove_reused_arrival_position_check', 'src/merge_capture.c',
     ' && arrival->position == e->position;',
     ';',
     'G_unresolved_reused_position', 'unresolved EXIT preserves reused arrival token'),
    ('untimed_TEXT_as_terminal_EXIT', 'src/core/merge.c',
     'if (published && !v->text && v->record.rank == PGWT_MERGE_EXIT && published->terminal_proved &&',
     'if (published && v->record.rank == PGWT_MERGE_EXIT && published->terminal_proved &&',
     'G_untimed_text_no_discharge', 'untimed TEXT never closes startup reader coverage'),
    ('double_local_writer_on_unresolved', 'src/merge_capture.c',
     '            observation(c, d, *s);\n            retain_snapshot(c);',
     '            observation(c, d, snap);\n            retain_snapshot(c);',
     'G_local_writer_once', 'unresolved disposition counts local writer failure once'),
]


# Amendment and round-five guard mutations.
MUTANTS += [('missing_disposition_inventory_guard', 'src/merge_capture.c',
  '        if (!found) return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);\n',
  '        (void)found;\n', 'G_disposition_uninventoried', 'round five guard complete output'),
 ('finished_discovery_guard', 'src/merge_capture.c',
  'if (!c || !c->config.discovery_enabled || !a || c->finished) return -EINVAL;',
  'if (!c || !c->config.discovery_enabled || !a) return -EINVAL;', 'G_discovery_after_finish',
  'finished capture retains no new disposition'),
 ('disposition_per_inventory', 'src/merge_capture.c', '            if (!same_exit) continue;',
  '            if (((void)same_exit, v->pid != a->pid)) continue;', 'G_unresolved_reuse',
  'reused PID EXITs each consume their own disposition'),
 ('mapped_hold_initialization', 'src/merge_capture.c', '    *hold = false;\n    ma_table t = table(c);',
  '    ma_table t = table(c);', 'G_mapped_hold_reset', 'mapped clears stale caller hold'),
 ('uncovered_position_guard', 'src/core/merge.c',
  '    case PGWT_MERGE_UNCOVERED:\n'
  '        if (position(m, in->source, in->p, obs) == PGWT_MERGE_EXIT_REQUIRED)\n'
  '            return m->report.status;',
  '    case PGWT_MERGE_UNCOVERED:', 'G_uncovered_position', 'round five guard complete output'),
 ('endpoint_overflow_call_guard', 'src/merge_capture.c',
  '    if (!discovery_endpoint(close, increment, &endpoint)) return 0;',
  '    if (!discovery_endpoint(close, increment, &endpoint)) endpoint=0;', 'G_endpoint_call_overflow',
  'round five guard inventory state'),
 ('no_word_requires_quiet', 'src/core/merge.c', 'bool valid = no_word || quiet_witness_valid(w);',
  'bool valid = quiet_witness_valid(w);', 'N1', 'no-word supporting publication before commitment'),
 ('NULL_implies_permanent_role', 'src/core/merge.c', 'w->role && w->permanent_role_qualified &&',
  'w->role &&', 'N2', 'unqualified NULL remains pending with its pointer watch'),
 ('unqualified_role_11', 'src/core/merge.c', 'w->role && w->permanent_role_qualified &&',
  'w->role && (w->permanent_role_qualified || w->role==11) &&', 'N11',
  'unqualified NULL remains pending with its pointer watch'),
 ('NULL_read_before_enable', 'src/core/merge.c', 'w->read_at > w->enable &&', 'true &&', 'N3',
  'qualified no-word route retained'),
 ('ignore_initial_pointer_fires', 'src/core/merge.c', '!w->pointer && !w->fires &&', '!w->pointer &&',
  'N4', 'qualified no-word route retained'),
 ('no_word_equal_completeness', 'src/core/merge.c', 'W > a->discovery.baseline.t;',
  'W >= a->discovery.baseline.t;', 'N1', 'no-word confirmation waits beyond enable'),
 ('no_word_ignore_disabled_watch', 'src/core/merge.c',
  'w->visible_before_enable && w->continuous && w->completed &&',
  'w->visible_before_enable && w->completed &&', 'N12', 'qualified no-word route retained'),
 ('no_word_pointer_not_a_fault', 'src/core/merge.c', 'state->no_wait_word && (!d->pointer_continuous ||',
  'false && state->no_wait_word && (!d->pointer_continuous ||', 'N5',
  'complete amended output sequence'),
 ('no_word_revoke_at_detection', 'src/merge_capture.c',
  '.at=c->coverage_boot_start, .detected=d, .revoked=true}', '.at=d, .detected=d, .revoked=true}', 'N5',
  'revocation starts at boot and retains detection time'),
 ('no_word_initialization_handoff', 'src/merge_capture.c',
  '            c->startup_closed = c->census_complete = c->census_declared = false;',
  '            c->startup_closed = true; c->census_complete = c->census_declared = false;', 'N5',
  'amended closure commitment'),
 ('NULL_fire_counts_read_loss', 'src/core/merge.c', '            ++state->pointer_observations;',
  '            ++state->pointer_observations; if (!d->pointer_value) { ++obs.counters.value[3]; return '
  'fault(m, PGWT_MERGE_COUNTER_CHANGE, obs); }',
  'N6', 'complete amended output sequence'),
 ('no_word_exit_replaces_bound', 'src/core/merge.c',
  'planned->discovery.terminal_time = published->terminal_at;',
  'planned->discovery.terminal_time = published->terminal_at; if (planned->discovery.no_wait_word) '
  'planned->discovery.lower_bound=published->terminal_at;',
  'N6', 'amended lower bound time'),
 ('invent_no_word_terminal_TRANS', 'src/core/merge.c',
  '    if (r->rank == PGWT_MERGE_TRANS && a->discovery.no_wait_word) return false;\n', '',
  'G_nw_TRANS', 'round six complete output'),
 ('NULL_consumes_attempt', 'src/core/merge.c',
  '            state->pointer_watched = state->pointer_watched || d->no_word.continuous;',
  '            state->pointer_watched = state->pointer_watched || d->no_word.continuous; ++state->wait_word_attempts;', 'N2',
  'NULL consumes no wait-word attempts'),
 ('initialization_supplies_bound', 'src/core/merge.c',
  'if (d->pointer_read_ok && d->pointer_value) state->pointer_nonnull = true;',
  'if (d->pointer_read_ok && d->pointer_value) { state->pointer_nonnull = true; '
  'state->has_lower_bound=true; state->lower_bound=d->pointer_time; }',
  'N7', 'initialization is not a lower bound'),
 ('authentication_timeout_closes_gap', 'src/core/merge.c',
  '                state->waiting_limit_exceeded = true;',
  '                state->waiting_limit_exceeded = true;\n'
  '            state->has_lower_bound=true;state->lower_bound=state->authentication_deadline;',
  'N16', 'separate pending and census'),
 ('no_word_forgives_journal_loss', 'src/merge_capture.c',
  '    if (c->lifecycle_lost) c->census_complete = false;',
  '    if (false && c->lifecycle_lost) c->census_complete = false;', 'N9',
  'separate pending and census'),
 ('pointer_read_failure_is_NULL', 'src/core/merge.c', 'w->read_ok && !w->pointer &&',
  'true && !w->pointer &&', 'N15', 'failed required pointer read is not a NULL candidate'),
 ('no_word_in_A2_population', 'src/merge_capture.c',
  'if (pgwt_a2_observed_birth(&birth, c->coverage_boot_start) &&\n'
  '            from <= birth.fork_ns && birth.fork_ns < to) ++n;',
  'if ((birth.discovery && from <= c->coverage_boot_start && c->coverage_boot_start < to) || '
  '(pgwt_a2_observed_birth(&birth, c->coverage_boot_start) && from <= birth.fork_ns && birth.fork_ns < '
  'to)) ++n;',
  'N1', 'no-word population contribution unavailable'),
 ('no_word_missing_quantities_zero', 'src/core/merge.c',
  '        no_word_baseline.t = no_word_baseline.e = d->no_word.enable;',
  '        no_word_baseline.t = no_word_baseline.e = d->no_word.enable; '
  'no_word_baseline.cpu_base_valid=true;',
  'N1', 'no-word route creates no numerical or CPU seed'),
 ('no_word_always_increment_endpoint', 'src/merge_capture.c', '    bool increment = false;',
  '    bool increment = false; for(size_t i=0;i<c->inventory_count;++i){ pgwt_discovery_state '
  'state={0}; if(pgwt_merge_discovery_state(c->core,c->inventory[i].pid,c->inventory[i].cookie,&state) '
  '&& state.no_wait_word) increment=true; }',
  'N1', 'amended persisted endpoint'),
 ('no_word_suppresses_other_increment', 'src/merge_capture.c',
  'if (s.has_uncertified && s.uncertified_through == close) increment = true;',
  'if (s.has_uncertified && s.uncertified_through == close) increment = true;\n'
  '        for(size_t j=0;j<c->inventory_count;++j){ pgwt_discovery_state state={0}; '
  'if(pgwt_merge_discovery_state(c->core,c->inventory[j].pid,c->inventory[j].cookie,&state) && '
  'state.no_wait_word) increment=false; }',
  'N13', 'amended persisted endpoint'),
 ('no_word_commit_after_support_rejection', 'src/core/merge.c',
  '!m->config.discovery_support(m->config.sink_context, a->proof.pid, a->proof.g,\n'
  '                    &a->discovery.no_word_witness, &obs))\n'
  '                return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);',
  '!m->config.discovery_support(m->config.sink_context, a->proof.pid, a->proof.g,\n'
  '                    &a->discovery.no_word_witness, &obs)) { a->discovery.has_lower_bound=true; '
  'a->discovery.lower_bound=a->discovery.baseline.t; return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs); '
  '}',
  'N14', 'separate pending and census'),
 ('numeric_invents_inventory_lifetime', 'src/merge_capture.c',
  '    c->numeric[c->numeric_count++] = (struct discovery_numeric){pid, listed_at, failed_open, false};',
  '    c->numeric[c->numeric_count++] = (struct discovery_numeric){pid, listed_at, failed_open, false}; '
  'c->inventory[c->inventory_count++]=(struct discovery_inventory){.pid=pid};',
  'J3', 'repeated numeric observations allocate no lifetime or cookie'),
 ('journal_discharge_before_EXIT_eligibility', 'src/core/merge.c', 'a->terminal_at = d->terminal_time;',
  'a->terminal_at = d->terminal_time; a->discovery.terminal_released=true; '
  'a->discovery.terminal_time=d->terminal_time;',
  'J1', 'journal waits strict EXIT release'),
 ('gone_on_liveness_or_EPERM', 'src/merge_capture.c', 'after_open && r->gone_esrch &&', 'after_open &&',
  'J4', 'separate pending and census'),
 ('gone_before_failed_open', 'src/merge_capture.c', 'r->gone_at > c->numeric[i].failed_open',
  'r->gone_at >= c->numeric[i].listed_at', 'J21', 'separate pending and census'),
 ('gone_ignores_lifecycle_loss', 'src/merge_capture.c',
  '    if (!reliable || r->F0 > c->coverage_boot_start || r->fence < c->census_fence)',
  '    if (((void)reliable, false) || r->F0 > c->coverage_boot_start || r->fence < c->census_fence)',
  'J5', 'separate pending and census'),
 ('journal_without_completed_producer', 'src/merge_capture.c',
  'r->namespaces && r->history_complete && r->producers_complete &&',
  'r->namespaces && r->history_complete &&', 'J6', 'separate pending and census'),
 ('exit_start_equal_F0_is_preexisting', 'src/core/merge_adapter.c',
  'if (r->start >= r->F0) return PGWT_DISC_EXIT_MISSING_FORK;',
  'if (r->start > r->F0) return PGWT_DISC_EXIT_MISSING_FORK;', 'J7',
  'three-fact exit filter emits no EXIT'),
 ('missing_fork_not_counted', 'src/core/merge_adapter.c',
  'if (r->start >= r->F0) return PGWT_DISC_EXIT_MISSING_FORK;',
  'if (r->start >= r->F0) return PGWT_DISC_EXIT_NONMEMBER;',
  'J7', 'complete amended output sequence'),
 ('exit_nonleader_passes', 'src/core/merge_adapter.c',
  'if (r->tid != r->tgid || r->parent != r->postmaster)', 'if (r->parent != r->postmaster)', 'J8',
  'three-fact exit filter emits no EXIT'),
 ('exit_tombstone_passes', 'src/core/merge_adapter.c',
  '    if (r->tid != r->tgid || r->parent != r->postmaster)',
  '    if (r->discovery_map_tombstone) return PGWT_DISC_EXIT_PREEXISTING;\n'
  '    if (r->tid != r->tgid || r->parent != r->postmaster)',
  'J9', 'three-fact exit filter emits no EXIT'),
 ('exit_stale_discovery_identity_passes', 'src/core/merge_adapter.c',
  '    if (r->tid != r->tgid || r->parent != r->postmaster)',
  '    if (r->discovery_map_cookie) return PGWT_DISC_EXIT_PREEXISTING;\n'
  '    if (r->tid != r->tgid || r->parent != r->postmaster)',
  'J10', 'three-fact exit filter emits no EXIT'),
 ('exit_unknown_start_precedes_F0', 'src/core/merge_adapter.c',
  '    if (!r->start_known || !r->start || !r->clocks_qualified)\n'
  '        return PGWT_DISC_EXIT_UNKNOWN;\n',
  '', 'J11', 'three-fact exit filter emits no EXIT'),
 ('rebind_fork_in_discovery_inventory', 'src/merge_capture.c',
  '        } else if (classification == PGWT_DISC_EXIT_FORK || classification == '
  'PGWT_DISC_EXIT_NONMEMBER) {',
  '        } else if (classification == PGWT_DISC_EXIT_FORK || classification == '
  'PGWT_DISC_EXIT_NONMEMBER) {\n'
  '            if(classification == PGWT_DISC_EXIT_FORK) '
  '(void)pgwt_capture_discovery_inventory(c,a->pid,PGWT_DISCOVERY_COOKIE_MIN);',
  'J12', 'separate pending and census'),
 ('rebound_preexisting_dropped', 'src/merge_capture.c',
  'if (classification == PGWT_DISC_EXIT_PREEXISTING) {',
  'if (classification == PGWT_DISC_EXIT_PREEXISTING) { resolved_numeric(c,a->pid); return '
  'pgwt_capture_envelope(c,&check,d,s);',
  'J13', 'separate pending and census'),
 ('rebound_nonmember_pending', 'src/merge_capture.c',
  'classification == PGWT_DISC_EXIT_FORK || classification == PGWT_DISC_EXIT_NONMEMBER',
  'classification == PGWT_DISC_EXIT_FORK', 'J14', 'separate pending and census'),
 ('journal_exit_current_PID_cookie', 'src/merge_capture.c', '        semantic.cookie = cookie;',
  '        semantic.cookie = c->lifetime_count ? c->lifetimes[c->lifetime_count-1].g : cookie;', 'J15',
  'complete amended output sequence'),
 ('journal_ignores_start_identity', 'src/merge_capture.c',
  '        if (v->start_known && v->start != start) return -EINVAL;\n', '', 'J16',
  'complete amended output sequence'),
 ('journal_ignores_EXIT_timestamp', 'src/merge_capture.c',
  '        if (r->exit_time != a->terminal_time)\n'
  '            return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);\n',
  '', 'J17', 'timestamp contradiction faults before matched disposition'),
 ('journal_discharge_rejected_EXIT', 'src/core/merge.c',
  '} else if (!m->config.sink(m->config.sink_context, &out, &obs))\n'
  '            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);',
  '} else if (!m->config.sink(m->config.sink_context, &out, &obs)) { if(published && '
  'v->record.rank==PGWT_MERGE_EXIT) { published->discovery.terminal_released=true; '
  'published->discovery.terminal_time=v->record.t; } return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs); '
  '}',
  'J18', 'separate pending and census'),
 ('journal_discharge_VOIDed_suffix', 'src/core/merge.c', 'planned->exit_release_committed = true;',
  'planned->exit_release_committed = true; if(published->terminal_proved) '
  '{published->discovery.terminal_released=true;published->discovery.terminal_time=published->terminal_at;}',
  'J19', 'separate pending and census'),
 ('gone_suppresses_late_revocation', 'src/merge_capture.c',
  '            if (i == c->inventory_count) {\n                /* Observation alone',
  '            if (i == c->inventory_count && !c->numeric_count) {\n'
  '                /* Observation alone',
  'J20', 'separate pending and census'),
 ('start_age_overrides_fork_provenance', 'src/core/merge_adapter.c',
  '    if (r->fork_cookie && r->fork_cookie < PGWT_DISCOVERY_COOKIE_MIN)',
  '    if (r->start >= r->F0 && r->fork_cookie && r->fork_cookie < PGWT_DISCOVERY_COOKIE_MIN)', 'J22',
  'observed fork provenance wins before age comparison'),
 ('unresolved_corrupts_resolved_pending', 'src/merge_capture.c',
  'if (!v->cookie || !pgwt_merge_discovery_state(c->core, v->pid, v->cookie, &state) ||',
  'bool unresolved_exit=false; for(size_t j=0;j<c->disposition_count;++j) '
  'if(c->dispositions[j].pid==v->pid && c->dispositions[j].unresolved) unresolved_exit=true;\n'
  '        if (unresolved_exit || !v->cookie || !pgwt_merge_discovery_state(c->core, v->pid, v->cookie, '
  '&state) ||',
  'G_unresolved_resolved_lifetime', 'round five guard inventory state'),
 ('unresolved_blocks_other_closure', 'src/merge_capture.c',
  '            if (!c->startup_closed) c->unresolved_startup_exit = true;',
  '            c->unresolved_startup_exit = true; c->startup_closed = false;',
  'G_unresolved_resolved_lifetime', 'round five guard inventory state'),
 ('no_word_individual_quantities_zero', 'src/merge_capture.c',
  '.present=true, .numerical_available=false, .measured_population_member=false}',
  '.present=true, .numerical_available=true, .measured_population_member=false}', 'N1',
  'individual quantities unavailable and measured population excluded'),
 ('census_ignores_init_loss', 'src/merge_capture.c', 'static const size_t lifecycle[] = {0, 1, 2, 10};',
  'static const size_t lifecycle[] = {0, 2, 10};', 'G_census_counter_class',
  'only named lifecycle losses invalidate census'),
 ('census_ignores_exit_loss', 'src/merge_capture.c', 'static const size_t lifecycle[] = {0, 1, 2, 10};',
  'static const size_t lifecycle[] = {0, 1, 10};', 'G_census_counter_class',
  'only named lifecycle losses invalidate census'),
 ('census_invalidated_by_nonlifecycle_loss', 'src/merge_capture.c',
  'static const size_t lifecycle[] = {0, 1, 2, 10};',
  'static const size_t lifecycle[] = {0, 1, 2, 10, 3, 14, 11};', 'G_census_counter_class',
  'only named lifecycle losses invalidate census'),
 ('journal_accepts_unknown_retained_start', 'src/merge_capture.c',
  'v->cookie == a->cookie && !v->start_known)', 'v->cookie == a->cookie && false)',
  'G_journal_unknown_start', 'round five guard complete output'),
 ('no_word_invents_A2_omission', 'src/merge_capture.c',
  'return pgwt_a2_omission_population(&l, c->coverage_boot_start, u);',
  'if (l.discovery) return PGWT_A2_POPULATION_OK; return pgwt_a2_omission_population(&l, '
  'c->coverage_boot_start, u);',
  'N1', 'no-word lifetime contributes no A2 omission'),
 ('numeric_forgets_failed_open_update', 'src/merge_capture.c',
  'if (failed_open > c->numeric[i].failed_open) c->numeric[i].failed_open = failed_open;',
  '(void)failed_open;', 'J2', 'separate pending and census'),
 ('numeric_resolution_qualifies_Q', 'src/merge_capture.c',
  '    if (c->mode != PGWT_MERGE_N)\n'
  '        return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);\n',
  '', 'G_numeric_resolution_in_Q', 'round five guard complete output')]




MUTANTS += [
    ('numeric_resolution_repairs_revoked_census', 'src/merge_capture.c',
     '                c->census_complete = c->census_declared = false;',
     '                c->census_complete = false;',
     'G_resolution_after_late_revocation', 'round five guard inventory state'),
]

# Reviewer round-5 survivors, with individually reached, complete-output cases.
MUTANTS += [('n1_fence_before_read',
  'src/core/merge.c',
  'w->fence >= w->read_at &&',
  'true &&',
  'G_nw_fence_before_read',
  'round six complete output'),
 ('n2_handles_differ',
  'src/core/merge.c',
  'w->handle0 == w->handle1 && w->role &&',
  'w->role &&',
  'G_nw_handles_differ',
  'round six complete output'),
 ('n4_no_binding',
  'src/core/merge.c',
  'w->counters_unchanged && w->ownership && w->binding;',
  'w->counters_unchanged;',
  'G_nw_no_binding',
  'round six complete output'),
 ('n5_confirm_role_mismatch',
  'src/core/merge.c',
  '            a->discovery.no_word_witness.role != d->no_word.role ||\n',
  '',
  'G_nw_confirm_role',
  'round six complete output'),
 ('n7_pointer_discontinuity_ok',
  'src/core/merge.c',
  'if (state->no_wait_word && (!d->pointer_continuous ||\n'
  '                (d->pointer_read_ok && d->pointer_value))) {',
  'if (state->no_wait_word && (\n                (d->pointer_read_ok && d->pointer_value))) {',
  'G_pointer_break_after_bound',
  'round six complete output'),
 ('n10_resolution_ignores_counter_delta',
  'src/merge_capture.c',
  '    if (reliable) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)\n'
  '        if (before.counters.value[i] != c->observation.counters.value[i]) reliable = false;\n',
  '',
  'G_resolution_delta_J',
  'round six complete census and pending'),
 ('n11_gone_fence_before_kill',
  'src/merge_capture.c',
  'r->gone_esrch && r->fence >= r->gone_at &&',
  'r->gone_esrch &&',
  'G_gone_fence_before_gone',
  'round six complete census and pending'),
 ('n12_gone_ignores_journal',
  'src/merge_capture.c',
  'r->fence >= r->gone_at && !r->journal_eligible)',
  'r->fence >= r->gone_at)',
  'G_gone_journal_precedence',
  'round six complete census and pending'),
 ('n13_rebind_ignores_journal_precedence',
  'src/merge_capture.c',
  'if (!r->binding || !r->handle || r->journal_eligible)',
  'if (!r->binding || !r->handle)',
  'G_rebind_journal_precedence',
  'round six complete census and pending'),
 ('n14_rebind_without_binding',
  'src/merge_capture.c',
  'if (!r->binding || !r->handle || r->journal_eligible)',
  'if (r->journal_eligible)',
  'G_rebind_no_binding',
  'round six complete census and pending'),
 ('n15_journal_without_named_exit',
  'src/merge_capture.c',
  '            !a->names_exit || !a->exit_position)\n',
  '            false)\n',
  'G_journal_unnamed',
  'round six complete census and pending'),
 ('n16_journal_tgid_mismatch_ok',
  'src/merge_capture.c',
  '        if (r->tgid != a->pid)\n'
  '            return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);\n',
  '',
  'G_journal_wrong_tgid',
  'round six complete output'),
 ('n17_numeric_exit_uncovered',
  'src/merge_capture.c',
  '            if (c->numeric[i].pid == pid && !c->numeric[i].resolved) uncovered = false;',
  '            (void)pid;',
  'G_numeric_EXIT_before_J',
  'numeric EXIT retained before journal'),
 ('n19_F0_after_boot_ok',
  'src/merge_capture.c',
  'if (!reliable || r->F0 > c->coverage_boot_start || r->fence < c->census_fence)',
  'if (!reliable || r->fence < c->census_fence)',
  'G_resolution_F0_after_boot',
  'round six complete census and pending'),
 ('n20_fence_before_census_ok',
  'src/merge_capture.c',
  'if (!reliable || r->F0 > c->coverage_boot_start || r->fence < c->census_fence)',
  'if (!reliable || r->F0 > c->coverage_boot_start)',
  'G_resolution_fence_before_census',
  'round six complete census and pending'),
 ('n21_discovery_cookie_is_fork',
  'src/core/merge_adapter.c',
  'if (r->fork_cookie && r->fork_cookie < PGWT_DISCOVERY_COOKIE_MIN)',
  'if (r->fork_cookie)',
  'G_discovery_cookie_not_fork',
  'round six complete census and pending'),
 ('n22_clocks_unqualified_ok',
  'src/core/merge_adapter.c',
  'if (!r->start_known || !r->start || !r->clocks_qualified)',
  'if (!r->start_known || !r->start)',
  'G_unqualified_clocks',
  'round six complete census and pending'),
 ('n24_no_word_TRANS_accepted',
  'src/core/merge.c',
  '    if (r->rank == PGWT_MERGE_TRANS && a->discovery.no_wait_word) return false;\n',
  '',
  'G_nw_TRANS',
  'round six complete output'),
 ('n25_quiet_confirm_after_no_word',
  'src/core/merge.c',
  '    } else if (a->discovery.no_wait_word ||\n'
  '               (a->discovery.pointer_watched && !a->discovery.pointer_nonnull)) {\n'
  '        return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n'
  '    }',
  '    }',
  'G_quiet_after_no_word',
  'round six complete output'),
 ('n26_try_after_admit_ok',
  'src/core/merge.c',
  '            if (state->admitted) return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n'
  '            state->pointer_watched',
  '            state->pointer_watched',
  'G_try_after_admit',
  'round six complete output'),
 ('n27_wait_attempt_after_admit_ok',
  'src/core/merge.c',
  '            if (state->admitted || (state->pointer_watched && !state->pointer_nonnull))',
  '            if ((state->pointer_watched && !state->pointer_nonnull))',
  'G_wait_after_admit',
  'round six complete output'),
 ('n29_late_inventory_keeps_census',
  'src/merge_capture.c',
  '        if (!numeric) c->census_declared = false;\n',
  '        (void)numeric;\n',
  'G_late_inventory_census',
  'round six complete census and pending'),
 ('n31_named_exit_wrong_source_ok',
  'src/merge_capture.c',
  'if (a->names_exit && (a->exit_source != c->source_count - 1 || !a->exit_position))',
  'if (a->names_exit && (!a->exit_position))',
  'G_named_exit_wrong_source',
  'round six complete output'),
 ('n32_mapped_disposition_time_unchecked',
  'src/merge_capture.c',
  '            if (v->terminal_at != at)\n'
  '                return pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, *s);\n'
  '            if (v->unresolved)',
  '            if (v->unresolved)',
  'G_named_exit_wrong_time',
  'round six complete output'),
 ('n34_admit_no_word_enable_mismatch',
  'src/core/merge.c',
  'd->e != d->no_word.enable || ',
  '',
  'G_nw_enable_mismatch',
  'round six complete output'),
 ('n35_wait_limit_any_time',
  'src/core/merge.c',
  'if (state->authentication_deadline && obs.d >= state->authentication_deadline)',
  'if (state->authentication_deadline)',
  'G_wait_limit_before_deadline',
  'deadline not exceeded before detection reaches it'),
 ('prior_nonnull_not_checked',
  'src/core/merge.c',
  '             a->discovery.pointer_nonnull || a->discovery.pointer_discontinuous ||',
  '             a->discovery.pointer_discontinuous ||',
  'G_pointer_before_admit',
  'round six complete output'),
 ('NULL_numerical_admit',
  'src/core/merge.c',
  '         (d->route == PGWT_DISC_NUMERICAL && a->discovery.pointer_watched &&\n'
  '             !a->discovery.pointer_nonnull)',
  '         false',
  'G_NULL_numerical_admit',
  'round six complete output'),
 ('prior_discontinuity_not_retained',
  'src/core/merge.c',
  '            if (!d->pointer_continuous) state->pointer_discontinuous = true;\n',
  '',
  'G_pointer_break_before_admit',
  'round six complete output'),
 ('TRY_erases_watch',
  'src/core/merge.c',
  'state->pointer_watched = state->pointer_watched || d->no_word.continuous;',
  'state->pointer_watched = d->no_word.continuous;',
  'G_try_cannot_erase_watch',
  'round six complete output'),
 ('quiet_to_no_word_handoff',
  'src/core/merge.c',
  'd->e != d->no_word.enable || a->quiet_pending',
  'd->e != d->no_word.enable',
  'G_quiet_before_no_word_admit',
  'round six complete output'),
 ('unresolved_gap_latch_removed',
  'src/merge_capture.c',
  '            if (!c->startup_closed) c->unresolved_startup_exit = true;\n',
  '',
  'G_unresolved_does_not_block_closure',
  'round five guard inventory state'),
 ('PID_binding_leaves_number_pending',
  'src/merge_capture.c',
  '    resolved_numeric(c, pid);\n    return 0;\n}\nint pgwt_capture_discovery_census',
  '    return 0;\n}\nint pgwt_capture_discovery_census',
  'G_PID_only_binding',
  'round six complete census and pending'),
 ('n10_resolution_ignores_counter_delta_G_resolution_delta_R',
  'src/merge_capture.c',
  '    if (reliable) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)\n'
  '        if (before.counters.value[i] != c->observation.counters.value[i]) reliable = false;\n',
  '',
  'G_resolution_delta_R',
  'round six complete census and pending'),
 ('n10_resolution_ignores_counter_delta_G_resolution_delta_G',
  'src/merge_capture.c',
  '    if (reliable) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)\n'
  '        if (before.counters.value[i] != c->observation.counters.value[i]) reliable = false;\n',
  '',
  'G_resolution_delta_G',
  'round six complete census and pending'),
 ('n4_no_binding_G_nw_no_ownership',
  'src/core/merge.c',
  'w->counters_unchanged && w->ownership && w->binding;',
  'w->counters_unchanged;',
  'G_nw_no_ownership',
  'round six complete output'),
 ('n14_rebind_without_binding_G_rebind_no_handle',
  'src/merge_capture.c',
  'if (!r->binding || !r->handle || r->journal_eligible)',
  'if (r->journal_eligible)',
  'G_rebind_no_handle',
  'round six complete census and pending'),
 ('n15_journal_without_named_exit_G_journal_no_position',
  'src/merge_capture.c',
  '            !a->names_exit || !a->exit_position)\n',
  '            false)\n',
  'G_journal_no_position',
  'round six complete output')]

MUTANTS += [
    ('no_word_fire_forgets_evidence', 'src/core/merge.c',
     'if (d->pointer_read_ok && d->pointer_value) state->pointer_nonnull = true;',
     'if (!state->no_wait_word && d->pointer_read_ok && d->pointer_value) state->pointer_nonnull = true;',
     'N5', 'non-NULL evidence survives its own fault'),
    ('terminal_before_admit_ignored', 'src/core/merge.c',
     '         (a->terminal_proved && d->e > a->terminal_at) ||\n', '',
     'G_terminal_before_admit', 'round six complete output'),
    ('terminal_before_confirm_ignored', 'src/core/merge.c',
     '    if ((a->terminal_proved && w->e > a->terminal_at) ||\n        !d->attachment_generation ||',
     '    if (!d->attachment_generation ||',
     'G_terminal_before_quiet', 'round six complete output'),
    ('terminal_after_claim_ignored', 'src/core/merge.c',
     '            if ((a->discovery.admitted || a->quiet_pending) &&\n'
     '                a->discovery.baseline.e > d->terminal_time)\n'
     '                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);\n', '',
     'G_quiet_before_terminal', 'round six complete output'),
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
        return dict(re.findall(r'^\| ([DQATVNJ][^ |]+) \| (.*?) \|$', path.read_text(), re.M))
    rows = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/ROWS.md')
    answers = markdown(ROOT / 'contracts/merge-v1/oracle-discovery/EXPECTED.md')
    table = []
    for line in (ROOT / 'tests/discovery_oracle_rows.h').read_text().splitlines():
        if line.startswith('    {'):
            table.append(json.loads('[' + line.strip()[1:-2] + ']'))
    ids = [row[0] for row in table]
    assert len(ids) == len(set(ids)) == 108
    assert set(ids) == set(rows) == set(answers), 'oracle row IDs drifted'
    assert all(inputs == rows[key] and answer == answers[key] for key, inputs, answer in table)
    expected_text = (ROOT / 'tests/discovery_oracle_expected.h').read_text()
    structured_rows = re.findall(r'^    \{"([DQATVNJ][^"]+)", "([^"]*)",', expected_text, re.M)
    structured = dict(structured_rows)
    assert len(structured_rows) == len(structured) == len(rows), 'duplicate or missing structured row'
    assert set(structured) == set(rows), 'structured expected row IDs drifted'
    states = dict((key, (int(persisted), int(pending), int(abandoned)))
        for key, persisted, pending, abandoned in re.findall(
            r'^    \{"([DQATVNJ][^"]+)", "[^"]*", -?\d+, (-?\d+), -?\d+, (\d+), \'[FQN]\', (?:true|false), (\d+)\},$',
            expected_text, re.M))
    amended_states = dict((key, (int(persisted), int(lifetimes)+int(numeric), 0))
        for key, persisted, lifetimes, numeric in re.findall(
            r'^    \{"([NJ][^"]+)", "[^"]*", -?\d+, (-?\d+), -?\d+, (\d+), (\d+), (?:true|false)\},$',
            expected_text, re.M))
    census_flags = dict(re.findall(
        r'^    \{"([NJ][^"]+)", "[^"]*", -?\d+, -?\d+, -?\d+, \d+, \d+, (true|false)\},$',
        expected_text, re.M))
    pending_parts = dict((key, (int(lifetime), int(numeric)))
        for key, lifetime, numeric in re.findall(
            r'^    \{"([NJ][^"]+)", "[^"]*", -?\d+, -?\d+, -?\d+, (\d+), (\d+), (?:true|false)\},$',
            expected_text, re.M))
    states.update(amended_states)
    assert set(states) == set(rows), 'structured state row IDs drifted'
    for key, answer in answers.items():
        codes = re.findall(r'`([^`]+)`', answer)
        sequence = next((code for code in codes if '→' in code or re.match(r'VOID\d+', code)), None)
        if sequence is None:
            sequence = next((code for code in codes if re.fullmatch(r'F\d+', code)), '')
        if key == 'D10':
            sequence = 'D12[U] → D15[T] → F15 → F30'
        if key == 'T1':
            sequence = 'F18 A20[T] F20'
        if key == 'T3':
            sequence = 'F18 A20[T] F20 VOID20[C0,unknown_evidence]'
        sequence = ' '.join(sequence.replace('→', ' ').split())
        sequence = re.sub(r'\bF(\d+)\b', r'F\1[C0]', sequence)
        if key.startswith('V'):
            sequence = sequence.replace('F19[C0]', 'F19[' + ('Ce' if key in ('V1', 'V2') else 'Cw') + ']')
        assert structured[key] == sequence, (key, structured[key], sequence)
        gap_ends = re.findall(r'G\((open|\d+)\)', answer)
        persisted = int(gap_ends[-1]) if gap_ends and gap_ends[-1] != 'open' else -1
        pending = re.findall(r'P=(\d+)(?:\+(\d+))?', answer)
        abandoned = re.findall(r'abandoned=(\d+)', answer)
        if key in amended_states:
            assert pending_parts[key] == tuple(int(part) for part in pending[-1]), key
            census = re.findall(r'census=(known|unknown)', answer)
            assert census_flags[key] == ('true' if census[-1] == 'known' else 'false'), key
        assert states[key] == (persisted, sum(int(part or 0) for part in pending[-1]) if pending else 1,
                               int(abandoned[-1]) if abandoned else 0), (key, states[key])
    print('discovery frozen table: 108/108 IDs and verbatim inputs/answers match', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutants', action='store_true')
    parser.add_argument('--mutant', action='append', choices=[m[0] for m in MUTANTS], default=[])
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
        if args.mutants or args.mutant:
            selected = [m for m in MUTANTS if args.mutants or m[0] in args.mutant]
            for name, path, before, after, row, assertion in selected:
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
            print(f'discovery mutants: {len(selected)}/{len(selected)} RED; built, reached named row, intended assertion', flush=True)
    print('Physical producer assertions not encoded: see tests/discovery_oracle_assertions.txt', flush=True)


if __name__ == '__main__':
    main()
