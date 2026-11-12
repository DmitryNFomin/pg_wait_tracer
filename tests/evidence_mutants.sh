#!/usr/bin/env bash
# Real edits to temporary source copies. Build/sanitizer/harness failures never
# count as kills. Frozen answers and the checkout's sources stay untouched.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -B - "$@" <<'PY'
import importlib.util
import contextlib
import io
from pathlib import Path
import re
import shutil
import sys
import tempfile

root = Path.cwd()
spec = importlib.util.spec_from_file_location("evidence_differential", root / "tests/test_core_evidence_differential.py")
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)

# (name, source text, replacement, intended case, required first mismatch)
mutants = [
    ("E1_no_attach", "out->projection.waits = out->waits;", """
    if (!out->projection.coverage_count && !out->projection.invalid_fields) {
        out->coverage = (pgwt_core_span){out->projection.life_start, UINT64_MAX};
        out->projection.coverage = &out->coverage;
        out->projection.coverage_count = 1;
    }
    out->projection.waits = out->waits;""", "08_attach_failure", "wait_time"),
    ("E2_eof_frontier", "free(history);", """
    if (target_capture && e->coverage_count && n && file->records[n-1].t > out->coverage.end) {
        out->coverage.end = file->records[n-1].t;
        if (e->has_open) e->open.as_of = out->coverage.end;
    }
    free(history);""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E3_transition_frontier", "free(history);", """
    for (size_t j = 0; target_capture && e->coverage_count && j < count; ++j) {
        if (history[j].record->t > out->coverage.end) {
            out->coverage.end = history[j].record->t;
            if (e->has_open) e->open.as_of = out->coverage.end;
        }
    }
    free(history);""", "16_crash_with_uncertified_suffix", "wait_time"),
    ("E4_frontier_close", "free(history);", """
    if (target_capture && e->has_open) {
        out->waits[e->wait_count++] = (pgwt_core_wait){e->open.event, e->open.start, e->open.as_of};
        e->has_open = false;
    }
    free(history);""", "17_wait_spans_frontiers", "wait_time"),
    ("E5_merge_zero_waits", "if (entry_known)", """if (entry_known &&
                !(entry == t->t && waits &&
                  candidate[waits - 1].start == entry &&
                  candidate[waits - 1].end == t->t &&
                  same(candidate[waits - 1].event, current)))""",
     "25_distinct_zero_length_waits", "completed_waits"),
    ("E6_trust_unqualified", "if (!file->qualified) continue;", "; /* trust certificate-shaped records */",
     "27_preempt_rt_unqualified", "wait_time"),
    ("E7_trust_legacy", "free(history);", """
    if (file->version < 5 && count) {
        e->invalid_fields = 0;
        e->life_start = history[0].record->t - history[0].record->duration;
        out->coverage = (pgwt_core_span){e->life_start, history[count - 1].record->t};
        e->coverage = &out->coverage;
        e->coverage_count = 1;
        for (size_t j = 0; j < count; ++j) {
            const pgwt_evidence_record *tr = history[j].record;
            if (!null_event(tr->old_event))
                out->waits[e->wait_count++] = (pgwt_core_wait){tr->old_event, tr->t - tr->duration, tr->t};
        }
        const pgwt_evidence_record *last = history[count - 1].record;
        e->has_open = !null_event(last->new_event);
        e->open = (pgwt_core_open_wait){last->new_event, last->t, last->t};
    }
    free(history);""", "28_legacy_v2", "wait_time"),
    ("E8_shift_coverage", "run->coverage = (pgwt_core_span){trimmed, endpoint};",
     "run->coverage = (pgwt_core_span){trimmed + 1, endpoint};",
     "17_wait_spans_frontiers", "projection.coverage[0][0]"),
    ("E9_accept_unknown_kind", 'return unsupported(why, "unsupported record kind");',
     'continue; /* ignore unknown storage kinds */',
     "native_unsupported_kind", "unsupported"),
]

differential_mutants = [
    ("E10_no_physical_prefix", "history[j].order >= i || ", "",
     "transition_after_frontier", "projection.waits.length"),
    ("E11_no_pid_check", "if (r->kind != PGWT_EVIDENCE_FRONTIER && r->pid != in->pid)",
     "if (0)", "refuse_other_pid_transition", "keys"),
    ("E12_no_duration_check", "(entry_known && t->t - t->duration != entry)", "false",
     "inconsistent_duration", "keys"),
    ("E13_no_old_state_check", "!same(t->old_event, current) ||", "",
     "inconsistent_old_state", "keys"),
    ("E14_no_transition_lifetime", "start <= t->t &&", "true &&",
     "transition_before_fork", "projection.coverage.length"),
    ("E15_no_attach_lifetime", "if (attach->t < life->start || (life->has_exit && attach->t >= life->exit)) {", "if (0) {",
     "refuse_attach_before_fork", "keys"),
    ("E16_no_frontier_advance", "if (has_frontier && r->t <= frontier) {", "if (0) {",
     "refuse_nonadvancing_frontier_15", "keys"),
]


# U1/U2 semantic guard inventory, one independent deletion per row. This also
# repeats the U1 prefix/state gates touched by the replay refactor. Allocation,
# pointer checks and loop mechanics are not evidence-semantic deletions.
# --guard-sweep runs BOTH full suites for every deletion; the default mode
# proves the named distinguishing assertion, never a crash or sanitizer error.
guard_mutants = [
    ('G01_target_capture_count', 'if (same(f->capture, in->capture)) ++targets;', '++targets;', '24_restart_same_pid', 'keys'),
    ('G02_rotation_is_not_restart', 'same(in->files[j].capture, f->capture) || ', '', 'refuse_restart_duplicate_non_target', 'keys'),
    ('G03_restart_clock_domain', '!same(in->files[j].boot, f->boot)', 'false', 'refuse_restart_other_boot', 'keys'),
    ('G04_observed_requires_fork', '!r->has_fork || r->start != r->fork', 'r->start != r->fork', 'refuse_life_observed_no_fork', 'keys'),
    ('G05_observed_start_is_fork', '!r->has_fork || r->start != r->fork', '!r->has_fork', 'refuse_life_observed_fork_mismatch', 'keys'),
    ('G06_provisional_has_no_fork', ': r->has_fork) ||', ': false) ||', 'refuse_life_provisional_with_fork', 'keys'),
    ('G07_exit_not_before_start', 'r->exit < r->start || ', '', 'refuse_life_exit_before_start', 'keys'),
    ('G08_exit_not_after_observation', 'r->exit > r->t', 'false', 'refuse_life_exit_after_observation', 'keys'),
    ('G09_attachment_kind', 'if (p->kind == PGWT_EVIDENCE_ATTACH_OK) {', 'if (false) {', '01_ordinary_run', 'keys'),
    ('G10_attachment_generation', 'runs[h].generation == k && (!runs[h].end', '(!runs[h].end', '02_pid_reuse_delayed_life', 'keys'),
    ('G11_reserved_new_event', ' && !exit_event(r->new_event)', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G12_key_fork_presence', 'a->has_fork == b->has_fork && ', '', 'reuse_provisional_then_observed', 'projection.coverage.length'),
    ('G13_key_fork_timestamp', 'a->fork == b->fork', 'true', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G14_life_start_immutable', 'g->life->start != r->start ||', '', 'life_lower_bound_changed', 'projection.life'),
    ('G15_life_exit_not_retracted', '!r->has_exit || ', '', 'life_zero_exit_retracted', 'projection.life'),
    ('G16_life_exit_immutable', 'g->life->exit != r->exit', 'false', 'life_exit_changed', 'projection.life'),
    ('G17_life_exit_update_requires_previous_exit', 'g->life->has_exit && ', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G18_discovery_is_first_life', 'if (!g->life) g->discovery = r->t;', 'g->discovery = r->t;', 'provisional_duplicate_discovery', 'projection.coverage.length'),
    ('G19_final_life_version', 'file->version == 5 && r->kind', 'r->kind', 'legacy_life_shaped_records', 'projection.life'),
    ('G20_final_life_kind', 'r->kind == PGWT_EVIDENCE_LIFE &&\n            r->has_fork', 'r->has_fork', '02_pid_reuse_delayed_life', 'projection.life'),
    ('G21_final_key_fork_presence', 'r->has_fork == in->has_fork && ', '', 'reuse_invalid_old_life', 'projection.life'),
    ('G22_final_key_fork_timestamp', 'r->fork == in->fork', 'true', '02_pid_reuse_delayed_life', 'projection.life'),
    ('G23_life_capture_isolation', 'if (target_capture && final.life', 'if (final.life', 'restart_life_exit_isolation', 'projection.life[1]'),
    ('G24_invalid_final_life', 'final.life && !final.invalid', 'final.life', 'life_exit_retracted', 'projection.life'),
    ('G25_prefix_life_dispatch', 'if (r->kind == PGWT_EVIDENCE_LIFE)\n            lifetime', 'if (true)\n            lifetime', '01_ordinary_run', 'projection.coverage.length'),
    ('G26_prefix_attach_dispatch', '(p->kind != PGWT_EVIDENCE_ATTACH_OK && p->kind != PGWT_EVIDENCE_END)', '(p->kind != PGWT_EVIDENCE_END)', '01_ordinary_run', 'projection.coverage.length'),
    ('G27_frontier_dispatch', 'if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;', '', '02_pid_reuse_delayed_life', 'keys'),
    ('G28_assignment_cutoff', ' || t->t > frontier', '', 'end_future_unassigned', 'projection.coverage[0][1]'),
    ('G29_assignment_invalid_life', 'if (!life || g[k].invalid) continue;', 'if (!life) continue;', 'reuse_invalid_old_life', 'projection.coverage.length'),
    ('G30_provisional_discovery_bound', 'life->has_fork ? life->fork : g[k].discovery', 'life->fork', 'provisional_edge_before_discovery', 'projection.coverage.length'),
    ('G31_exit_matching_timestamp', 'life->has_exit && life->exit == t->t', 'life->has_exit', 'exit_unmatched_sentinel', 'projection.coverage.length'),
    ('G32_exit_requires_life_exit', 'life->has_exit && life->exit == t->t', 'life->exit == t->t', 'exit_without_life_exit_at_zero', 'projection.coverage.length'),
    ('G33_ordinary_fork_bound', 'start <= t->t && ', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G34_ordinary_exit_bound', ' && (!life->has_exit || t->t < life->exit)', '', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G35_unique_assignment', 'if (choices != 1) blocked = true;', '', 'reuse_missing_old_exit', 'projection.coverage.length'),
    ('G36_tracking_invalid_life', 'if (!life || g[k].invalid || !attach) continue;', 'if (!life || !attach) continue;', 'invalid_late_life_attachment', 'keys'),
    ('G37_attach_lower_bound', 'attach->t < life->start || ', '', 'refuse_attach_before_fork', 'keys'),
    ('G38_attach_exit_bound', 'life->has_exit && attach->t >= life->exit', 'false', 'exit_attach_at_exit', 'keys'),
    ('G39_unresolved_identity_blocks', 'blocked || attach->t > frontier', 'attach->t > frontier', 'reuse_missing_old_exit', 'projection.coverage.length'),
    ('G40_attach_before_frontier', 'blocked || attach->t > frontier', 'blocked', 'future_reread_mismatch_pending', 'keys'),
    ('G41_endpoint_requires_exit', 'life->has_exit && life->exit < frontier ?', 'life->exit < frontier ?', '01_ordinary_run', 'projection.coverage.length'),
    ('G42_endpoint_capped_by_frontier', 'life->has_exit && life->exit < frontier ?', 'life->has_exit ?', 'exit_after_frontier', 'projection.coverage.length'),
    ('G43_assigned_generation', 'if (assigned[j] != k) continue;', '', 'run_touching_other_first_generation', 'keys'),
    ('G44_exit_is_terminal', 'terminal || !same(t->old_event, current)', '!same(t->old_event, current)', 'duplicate_nonwaiting_exit', 'keys'),
    ('G45_coverage_capture_isolation', 'bool target = target_capture && key(', 'bool target = key(', 'restart_same_key_reversed', 'projection.coverage[0][0]'),
    ('G46_coverage_generation_isolation', 'key(g[k].identity, &(pgwt_evidence_record){\n                .has_fork = in->has_fork, .fork = in->fork})', 'true', 'reuse_query_old_window', 'projection.coverage[0][0]'),
    ('G47_invalid_life_suppresses_coverage', 'if (!target || final.invalid) continue;', 'if (!target) continue;', 'life_exit_retracted', 'projection.coverage.length'),
    ('G48_fallback_requires_known_exit', 'e->has_life_end && endpoint', 'endpoint', '01_ordinary_run', 'projection.coverage.length'),
    ('G49_fallback_reaches_exit', 'endpoint >= e->life_end && ', '', 'exit_missing_terminal_fallback', 'projection.coverage.length'),
    ('G50_fallback_requires_missing_terminal', ' && !terminal) continue;', ') continue;', '03_exit_inside_window', 'projection.coverage.length'),
    ('G51_life_start_known_mode', '(r->start_known ? (!r->has_fork || r->start != r->fork) : r->has_fork)', '(!r->has_fork || r->start != r->fork)', '06_preexisting_before_capture', 'keys'),
    ('G52_exit_validation_presence', 'r->has_exit && (r->exit < r->start || r->exit > r->t)', '(r->exit < r->start || r->exit > r->t)', '02_pid_reuse_delayed_life', 'keys'),
    ('G53_generation_lookup', 'if (key(g[i].identity, r)) return i;', 'return i;', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G54_identity_match', 'if (matches) { assigned[j] = k; ++choices; }', '{ assigned[j] = k; ++choices; }', '02_pid_reuse_delayed_life', 'projection.coverage.length'),
    ('G55_assignment_exit_presence', '!life->has_exit || t->t < life->exit', 't->t < life->exit', '01_ordinary_run', 'projection.coverage.length'),
    ('G56_attach_exit_presence', 'life->has_exit && attach->t >= life->exit', 'attach->t >= life->exit', '01_ordinary_run', 'keys'),
    ('G57_exit_clears_open', 'current = terminal ? (pgwt_core_event){0} : t->new_event;', 'current = t->new_event;', '03_exit_inside_window', 'projection.open'),
    ('G58_key_ignores_absent_fork_bytes', '(!a->has_fork || a->fork == b->fork)', '(a->fork == b->fork)', 'native_provisional_fork_bytes', 'projection'),
    ('G59_final_ignores_absent_fork_bytes', '(!r->has_fork || r->fork == in->fork)', '(r->fork == in->fork)', 'native_provisional_fork_bytes', 'projection'),
    ('G60_new_event_validation', '(!state(r->new_event) && !exit_event(r->new_event))', 'false', 'refuse_transition_empty_new', 'keys'),
    ('G61_observed_fork_not_discovery', 'life->has_fork ? life->fork : g[k].discovery', 'g[k].discovery', 'observed_life_delayed_observation', 'projection.coverage.length'),
    ('G62_capture_failure_propagates', 'if (status != PGWT_REPLAY_OK) goto done;', '', 'inconsistent_duration', 'keys'),
    ('G63_physical_prefix', 'history[j].order >= i || ', '', 'transition_after_frontier', 'projection.waits.length'),
    ('G64_frontier_advance', 'if (has_frontier && r->t <= frontier) {', 'if (false) {', 'refuse_nonadvancing_frontier_15', 'keys'),
    ('G65_duration_consistency', '(entry_known && t->t - t->duration != entry)', 'false', 'inconsistent_duration', 'keys'),
    ('G66_old_state_consistency', '!same(t->old_event, current) ||', '', 'inconsistent_old_state', 'keys'),
    ('G67_reread_reconciles_anchor', 'if (attach->reread && t->t == attach->t) continue;', '', 'transitions_through_reread', 'keys'),
    ('G68_qualification', 'if (!file->qualified) continue;', '', '27_preempt_rt_unqualified', 'projection.coverage.length'),
    ('G69_legacy_prefix', 'if (file->version != 5) continue;', '', 'legacy_life_shaped_records', 'projection.coverage.length'),
    ('G70_attach_reread_only', 'bool anchored = attach->reread;', 'bool anchored = true;', 'guard_attach_transition', 'projection.coverage.length'),
    ('G71_attach_seed_null', '!same(attach->seed, current)', 'false', 'guard_attach_seed_nonnull', 'keys'),
    ('G72_attach_state_null', 'pgwt_core_event current = attach->state;', 'pgwt_core_event current = attach->seed;', 'guard_attach_state_nonnull', 'keys'),
    ('G73_ordinary_exit_strict', 't->t < life->exit))', 't->t <= life->exit))', 'guard_ordinary_edge_then_terminal_at_exit', 'projection.coverage.length'),
    ('G74_transition_changes_state', '                       same(r->old_event, r->new_event) || r->duration > r->t', '                       r->duration > r->t', 'guard_same_event_transition', 'keys'),
    ('G75_duration_within_timestamp', 'same(r->old_event, r->new_event) || r->duration > r->t', 'same(r->old_event, r->new_event)', 'guard_duration_exceeds_timestamp', 'keys'),
    ('G76_old_state_valid', '} else if (!state(r->old_event) || ', '} else if (', 'guard_empty_old_state', 'keys'),
    ('G77_life_start_not_future', ' ||\n                    r->start > r->t ||', ' ||', 'guard_life_start_after_observation', 'keys'),
    ('G78_attach_zero_counters', 'if (!zero_counters(&r->counters))\n                    return unsupported(why, "unsupported attachment counters");', '', 'guard_attach_nonzero_counter', 'keys'),
    ('G79_first_physical_discovery', '    if (!g->life) g->discovery = r->t;\n', '    if (!g->life || r->t < g->discovery) g->discovery = r->t;\n', 'guard_first_physical_discovery', 'projection.coverage.length'),
]

# U3 inventory includes every new/changed semantic branch and each term of
# compound guards. The two +1 assignments have separate semantic mutants;
# overflow is rejected by portion, without saturating or projecting wraparound.
u3_guards = [
    ('G80_attachment_seed_state', '!state(r->seed) || ', '', 'native_invalid_seed', 'validation'),
    ('G81_attachment_verified_state', '!state(r->state)', 'false', 'native_invalid_state', 'validation'),
    ('G82_reread_equality_method', 'attach->reread && !same(attach->seed, current)', '!same(attach->seed, current)', 'anchor_seed_is_not_old', 'keys'),
    ('G83_reread_seed_equality', '!same(attach->seed, current)', 'false', 'reread_seed_mismatch', 'keys'),
    ('G84_anchor_lower_bound', 'if (t->t < attach->t) continue;', '', 'anchor_prior_transition', 'projection.coverage.length'),
    ('G85_anchor_dispatch', 'if (!anchored) {', 'if (false) {', '09_seed_mismatch_then_transition', 'keys'),
    ('G86_anchor_exact_time', 'if (t->t != attach->t) break;', '', 'anchor_wrong_time', 'projection.coverage.length'),
    ('G87_anchor_state', 'if (!same(t->new_event, current)) {', 'if (false) {', 'anchor_wrong_state', 'keys'),
    ('G88_anchor_old_side_trim', 'if (!null_event(t->old_event)) {', 'if (false) {', '09_seed_mismatch_then_transition', 'projection.coverage[0][0]'),
    ('G89_anchor_trim_overflow', 'portion = t->t != UINT64_MAX;\n                        trimmed', 'portion = true;\n                        trimmed', 'anchor_trim_overflow', 'projection.coverage.length'),
    ('G90_reconcile_only_reread', 'attach->reread && t->t == attach->t', 't->t == attach->t', 'anchor_later_ties', 'projection.waits.length'),
    ('G91_reconcile_only_anchor_time', 'attach->reread && t->t == attach->t', 'attach->reread', '11_unknown_entry_close_boundary', 'projection.coverage.length'),
    ('G92_duration_requires_entry', 'entry_known && t->t - t->duration != entry', 't->t - t->duration != entry', 'unknown_close_duration_not_entry', 'keys'),
    ('G93_unknown_close_named_old', '!null_event(current) && !entry_known', '!entry_known', '01_ordinary_run', 'projection.coverage[0][0]'),
    ('G94_unknown_close_entry', '!null_event(current) && !entry_known', '!null_event(current)', '01_ordinary_run', 'projection.coverage[0][0]'),
    ('G95_append_known_only', 'if (entry_known)\n                    candidate', 'if (true)\n                    candidate', '01_ordinary_run', 'projection.waits.length'),
    ('G96_unknown_close_trim', 'if (unknown_close) {', 'if (false) {', '11_unknown_entry_close_boundary', 'projection.coverage.length'),
    ('G97_unknown_trim_overflow', 'portion = t->t != UINT64_MAX;\n                    trimmed', 'portion = true;\n                    trimmed', 'unknown_trim_overflow', 'projection.coverage.length'),
    ('G98_anchor_dependency', '!anchored || !portion || ', '!portion || ', 'anchor_missing_null_state', 'projection.coverage.length'),
    ('G99_representable_portion', '!anchored || !portion || ', '!anchored || ', '10_equal_nonzero_seed', 'projection.coverage.length'),
    ('G100_trim_before_endpoint', ' || trimmed > endpoint', '', 'unknown_close_at_frontier', 'projection.coverage.length'),
    ('G101_seed_entry_unknown', 'bool entry_known = false, portion', 'bool entry_known = true, portion', '11_unknown_entry_close_boundary', 'keys'),
    ('G102_seed_portion_unknown', 'portion = null_event(current);', 'portion = true;', '10_equal_nonzero_seed', 'projection.coverage.length'),
    ('G103_transition_needs_anchor', 'bool anchored = attach->reread;', 'bool anchored = true;', '09_seed_mismatch_then_transition', 'keys'),
    ('G104_reread_needs_no_transition', 'bool anchored = attach->reread;', 'bool anchored = false;', '01_ordinary_run', 'projection.coverage.length'),
    ('G105_anchor_null_has_no_entry', 'entry_known = !null_event(current);\n                    portion', 'entry_known = true;\n                    portion', 'anchor_null_then_wait', 'keys'),
    ('G106_new_null_has_no_entry', 'entry_known = !null_event(current);\n            }', 'entry_known = true;\n            }', '01_ordinary_run', 'projection.waits.length'),
    ('G107_open_requires_entry', 'run->has_open = entry_known;', 'run->has_open = false;', '09_seed_mismatch_then_transition', 'projection.open'),
    ('G108_anchor_named_has_entry', 'entry_known = !null_event(current);\n                    portion', 'entry_known = false;\n                    portion', '09_seed_mismatch_then_transition', 'projection.open'),
    ('G109_new_named_has_entry', 'entry_known = !null_event(current);\n            }', 'entry_known = false;\n            }', '01_ordinary_run', 'projection.coverage[0][0]'),
    ('G110_anchor_null_old_not_trimmed', 'if (!null_event(t->old_event)) {', 'if (true) {', 'anchor_old_null', 'projection.coverage[0][0]'),
]
# U2's former attachment-shape refusals now exercise U3's anchor and seed
# reconciliation rules with the same reviewer witnesses.
guard_mutants += u3_guards
guard_mutants.append((
    'R14_kind_check',
    'if (r->kind != PGWT_EVIDENCE_LIFE && r->kind != PGWT_EVIDENCE_ATTACH_OK &&',
    'if (false && r->kind != PGWT_EVIDENCE_ATTACH_OK &&',
    'native_unsupported_kind', 'unsupported'))

differential_mutants += [
    ('E17_seed_has_no_entry', 'bool entry_known = false, portion = null_event(current);', 'bool entry_known = !null_event(current), portion = true;', '10_equal_nonzero_seed', 'projection.coverage.length'),
    ('E18_anchor_entry_instant', 'entry = t->t;\n                    entry_known', 'entry = t->t + 1;\n                    entry_known', '09_seed_mismatch_then_transition', 'projection.open[1]'),
    ('E19_anchor_exclude_one_ns', 'trimmed = t->t + 1; /* Wrapped value is never projected. */\n                    }', 'trimmed = t->t;\n                    }', '09_seed_mismatch_then_transition', 'projection.coverage[0][0]'),
    ('E20_unknown_exclude_one_ns', 'trimmed = t->t + 1; /* Wrapped value is never projected. */\n                }', 'trimmed = t->t;\n                }', '11_unknown_entry_close_boundary', 'projection.coverage[0][0]'),
    ('E21_preserve_real_wait_start', '(pgwt_core_wait){current, entry, t->t}', '(pgwt_core_wait){current, entry < trimmed ? trimmed : entry, t->t}', '11_unknown_entry_close_boundary', 'projection.waits[0][1]'),
    ('E22_anchor_old_is_untrusted', 'continue; /* The anchor\'s old side is untrusted. */', '; /* consume the untrusted old side */', '09_seed_mismatch_then_transition', 'keys'),
]

# U4 semantic guards, removed independently. Pointer/allocation checks are not
# semantic guards. END assignment is resolved only by a physical FRONTIER.
u4_guards = [
    ('U401_END_kind', 'r->kind != PGWT_EVIDENCE_END)', 'true)', '18_detach_mid_wait', 'keys'),
    ('U402_END_reason', 'if (!same(r->end_reason, (pgwt_core_event){"detach", 6}) &&', 'if (false &&', 'native_end_reason', 'validation'),
    ('U403_END_detach', '(pgwt_core_event){"detach", 6}', '(pgwt_core_event){"other", 5}', 'end_reason_detach', 'keys'),
    ('U404_END_deescalate', '(pgwt_core_event){"deescalate", 10}', '(pgwt_core_event){"other", 5}', 'end_reason_deescalate', 'keys'),
    ('U405_END_stop', '(pgwt_core_event){"stop", 4}', '(pgwt_core_event){"other", 5}', 'end_reason_stop', 'keys'),
    ('U406_pending_once', 'processed[j] || ', '', '18_detach_mid_wait', 'keys'),
    ('U407_pending_END', ' && p->kind != PGWT_EVIDENCE_END)', ')', '18_detach_mid_wait', 'projection.coverage[0][1]'),
    ('U408_overlap_END_presence', '(!runs[h].end || runs[h].end->t > p->t)', '(!runs[h].end)', 'run_overlap_invalid', 'keys'),
    ('U409_overlap_END_order', 'runs[h].end->t > p->t', 'false', 'run_overlap_invalid', 'keys'),
    ('U410_END_generation', 'runs[h].generation == k && runs[h].attach->t', 'runs[h].attach->t', 'end_other_generation_known', 'keys'),
    ('U411_END_lower_bound', 'runs[h].attach->t <= p->t && ', '', 'end_before_attach', 'keys'),
    ('U412_END_once', ' && !runs[h].end)', ')', 'end_duplicate', 'keys'),
    ('U413_END_unique', 'if (choices != 1) {', 'if (false) {', 'end_duplicate', 'keys'),
    ('U414_END_cap', 'if (run->end && run->end->t < endpoint) endpoint = run->end->t;', '', 'end_caps_open', 'projection.coverage[0][1]'),
    ('U415_END_cap_presence', 'run->end && run->end->t < endpoint', 'false', 'end_caps_open', 'projection.coverage[0][1]'),
    ('U416_END_cap_frontier', 'run->end && run->end->t < endpoint', 'run->end', 'end_beyond_frontier', 'projection.coverage[0][1]'),
    ('U417_run_transition_endpoint', 'if (t->t > endpoint) continue;', '', 'end_excludes_later_close', 'projection.waits.length'),
    ('U418_run_touching_ties', 'if (touching) continue;', '', 'run_touching_tied_transition', 'keys'),
    ('U419_touching_first_generation', 'runs[x].generation == k && ', '', 'run_touching_other_first_generation', 'projection.coverage.length'),
    ('U420_touching_second_generation', 'runs[y].generation == k &&', 'true &&', 'run_touching_other_second_generation', 'projection.coverage[0][1]'),
    ('U421_touching_boundary', 'runs[x].end->t == runs[y].attach->t', 'false', 'run_touching_tied_transition', 'keys'),
    ('U422_touching_transition_generation', 'assigned[j] == k && history[j].record->t', 'history[j].record->t', 'run_touching_other_transition', 'projection.coverage.length'),
    ('U423_touching_transition_time', 'history[j].record->t == runs[y].attach->t', 'true', 'run_touching_no_tie', 'projection.coverage[0][0]'),
    ('U424_query_overlap', 'if (ao != bo) return ao > bo;', '', 'run_cross_boundary', 'projection.coverage[0][0]'),
    ('U425_query_malformed', 'q && !q->invalid_fields && nonempty(q->event) && q->from < q->to', 'q', 'run_invalid_event', 'projection.coverage[0][0]'),
    ('U426_query_invalid_fields', '!q->invalid_fields && ', '', 'run_missing_endpoint', 'projection.coverage[0][0]'),
    ('U427_query_event_shape', 'nonempty(q->event) && ', '', 'run_invalid_event', 'projection.coverage[0][0]'),
    ('U428_query_window', ' && q->from < q->to', '', 'run_invalid_window', 'projection.coverage[0][0]'),
    ('U429_query_start_lifetime', 'q->from > life->life_start ? q->from : life->life_start', 'q->from', 'run_query_before_life', 'projection.coverage[0][0]'),
    ('U430_query_end_lifetime', 'life->has_life_end && life->life_end < q->to ? life->life_end : q->to', 'q->to', 'run_empty_required_exit', 'projection.coverage[0][0]'),
    ('U431_query_full_preference', 'if (af != bf) return af;', '', 'run_empty_required_exit', 'projection.coverage[0][0]'),
    ('U432_query_exit_needed', 'life->has_life_end && q->from <= life->life_end && life->life_end < q->to', 'false', 'run_empty_required_exit', 'projection.coverage[0][0]'),
    ('U433_query_tie_start', 'if (a->coverage.start != b->coverage.start) return a->coverage.start < b->coverage.start;', '', 'run_equal_overlap', 'projection.coverage[0][0]'),
    ('U434_query_tie_end', 'if (a->coverage.end != b->coverage.end) return a->coverage.end < b->coverage.end;', '', 'run_tie_smallest_end', 'projection.coverage[0][1]'),
    ('U435_query_one_run', 'prefer(&runs[h], selected, e, query)', 'true', 'run_first_full', 'projection.coverage[0][0]'),
]
u4_guards += [
    ('U436_pending_invalid_life', 'if (!g[k].life || g[k].invalid) continue;', 'if (!g[k].life) continue;', 'invalid_life_pending_attachment', 'keys'),
    ('U437_END_shape_dispatch', 'r->kind == PGWT_EVIDENCE_END) {', 'false) {', '18_detach_mid_wait', 'keys'),
    ('U438_touching_distinct_runs', 'x != y && ', '', 'end_transition_point', 'projection.coverage.length'),
    ('U439_select_snapshot', 'runs[h].has_snapshot && ', '', 'anchor_missing_null_state', 'projection.coverage.length'),
    ('U440_query_exit_left', 'q->from <= life->life_end && ', '', 'run_query_wholly_after_exit', 'projection.coverage[0][0]'),
    ('U441_query_exit_right', 'q->from <= life->life_end && life->life_end < q->to', 'q->from <= life->life_end', 'run_query_wholly_before_life', 'projection.coverage[0][0]'),
    ('U442_query_clamp_empty', 'if (right < left) right = left;', '', 'run_query_wholly_before_life', 'projection'),
    ('U443_query_exit_endpoint_a', 'a->coverage.end == life->life_end && ', '', 'run_empty_required_exit', 'projection'),
    ('U444_query_exit_endpoint_b', 'b->coverage.end == life->life_end && ', '', 'run_empty_required_exit', 'projection'),
    ('U445_query_exit_terminal_a', ' && a->terminal', '', 'run_empty_required_exit', 'projection'),
    ('U446_query_exit_terminal_b', ' && b->terminal', '', 'run_empty_required_exit', 'projection'),
]
u4_guards.append((
    'U447_END_inclusive_transition', 'if (t->t > endpoint) continue;', 'if (t->t >= endpoint) continue;',
    'end_includes_tied_close', 'projection.waits.length'))
guard_mutants += u4_guards

# These deletions are algebraically redundant on reachable candidates, rather
# than missing witnesses. Every one still builds and runs BOTH entire suites.
# Any observed difference contradicts its proof and fails the harness.
equivalent_deletions = {
    'G36_tracking_invalid_life': 'pending resolution already requires valid LIFE; invalid generations receive no transitions and cannot project final evidence',
    'U428_query_window': 'from>=to gives zero overlap for every run; all preferences tie by time',
    'U429_query_start_lifetime': 'every run begins at/after LIFE.start; lower clipping cannot change overlap or the winner',
    'U433_query_tie_start': 'nonoverlapping runs have the same start/end time ordering, retained in append order',
    'U434_query_tie_end': 'equal-start runs are point/touching runs, whose endpoints already follow append order',
    'U442_query_clamp_empty': 'right<left makes all overlaps zero; wrapped required length cannot equal zero, preserving all ties',
    'U443_query_exit_endpoint_a': 'terminal can only be set by the unique LIFE.exit transition',
    'U444_query_exit_endpoint_b': 'terminal can only be set by the unique LIFE.exit transition',
    'U445_query_exit_terminal_a': 'a snapshot at LIFE.exit is retained only when terminal is certified',
    'U446_query_exit_terminal_b': 'a snapshot at LIFE.exit is retained only when terminal is certified',
}

differential_mutants += [
    ('E23_END_never_closes', 'run->has_open = entry_known;', 'if (run->end && entry_known) { saved = realloc(run->waits, (waits+1)*sizeof(*saved)); if (!saved) goto allocation_error; run->waits=saved; run->waits[run->wait_count++] = (pgwt_core_wait){current,entry,endpoint}; } run->has_open = entry_known && !run->end;', 'end_caps_open', 'projection.waits.length'),
    ('E24_END_never_changes_life', 'out->coverage = selected->coverage;', 'out->coverage = selected->coverage; if (selected->end) { e->has_life_end=true; e->life_end=selected->end->t; }', 'end_caps_open', 'projection.life[1]'),
    ('E26_tie_start_reverse', 'a->coverage.start < b->coverage.start', 'a->coverage.start > b->coverage.start', 'run_equal_overlap', 'projection.coverage[0][0]'),
    ('E27_tie_end_reverse', 'a->coverage.end < b->coverage.end', 'a->coverage.end > b->coverage.end', 'run_tie_smallest_end', 'projection.coverage[0][1]'),
    ('E28_waits_one_run', 'e->wait_count = selected->wait_count;', 'e->wait_count = selected->wait_count; for (size_t h=0; h<run_count; ++h) if (&runs[h] != selected && runs[h].has_snapshot && runs[h].wait_count) { memcpy(out->waits+e->wait_count,runs[h].waits,runs[h].wait_count*sizeof(*out->waits)); e->wait_count+=runs[h].wait_count; }', 'run_waits_isolated', 'projection.waits.length'),
    ('E29_END_append_order_is_not_time', 'if (t->t > endpoint) continue;', 'if (t->t > endpoint || (run->end && history[j].order > (size_t)(run->end-file->records))) continue;', 'end_delayed_transition', 'projection.open'),
    ('E25_END_requires_frontier', 'if (r->kind != PGWT_EVIDENCE_FRONTIER) continue;', 'if (r->kind != PGWT_EVIDENCE_FRONTIER && r->kind != PGWT_EVIDENCE_END) continue;', 'end_pending_eof', 'projection.coverage[0][1]'),
]

contract_guards = {'G58_key_ignores_absent_fork_bytes', 'G59_final_ignores_absent_fork_bytes',
                   'G80_attachment_seed_state', 'G81_attachment_verified_state', 'R14_kind_check', 'U402_END_reason'}


def differential_case(binary, work, case):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        status = diff.differential(binary, work, case)
    text = output.getvalue()
    # The differential rejects crashes, sanitizer errors and malformed output.
    valid = bool(re.search(r"^evidence differential: [01] reference cases, [01] unsupported cases, "
                           r"1 differences, seed=0x[0-9a-f]+$", text, re.M))
    return status, text, valid


def contract(binary):
    result = diff.execute([binary, diff.CONTRACT])
    output = result.stdout + result.stderr
    rows = re.findall(r"^evidence \S+ (?:PASS|UNSUPPORTED-OK|FAIL .+)$", output, re.M)
    valid = len(rows) == 40 and not diff.sanitizer_error(output) and " ERROR " not in output
    return result.returncode, output, valid


def full_differential(binary, work, corpus):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        status = diff.differential(binary, work, corpus=corpus)
    text = output.getvalue()
    valid = bool(re.search(r"^evidence differential: [0-9]+ reference cases, [0-9]+ unsupported cases, "
                           r"[0-9]+ differences, seed=0x[0-9a-f]+$", text, re.M))
    return status, text, valid


def main():
    if sys.argv[1:] not in ([], ["--guard-sweep"]):
        raise RuntimeError("usage: evidence_mutants.sh [--guard-sweep]")
    sweep = bool(sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-mutants-") as temp:
        work = Path(temp)
        base = work / "baseline"
        (base / "src").mkdir(parents=True)
        (base / "tests").mkdir()
        shutil.copytree(root / "src/core", base / "src/core")
        for name in ("test_core_evidence_contract.c", "evidence_json.c", "evidence_json.h"):
            shutil.copy2(root / "tests" / name, base / "tests" / name)
        binary = base / "contract-test"
        diff.build(base, binary)
        status, output, valid = contract(binary)
        if status != 0 or not valid:
            raise RuntimeError("baseline contract failed:\n" + output)
        corpus = diff.prepare_corpus(work)
        if diff.differential(binary, work, corpus=corpus):
            raise RuntimeError("baseline differential failed")
        failed = False
        all_mutants = guard_mutants if sweep else mutants + differential_mutants + guard_mutants
        differential_names = {m[0] for m in differential_mutants + guard_mutants} - contract_guards
        if sweep:
            print("guard | source line | contract | differential | intended assertion | result", flush=True)
        for name, old, new, case, field in all_mutants:
            directory = work / name
            shutil.copytree(base, directory, ignore=shutil.ignore_patterns("contract-test", "*.dSYM"))
            source = directory / "src/core/evidence_replay.c"
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name} needs exactly one mutation site")
            line = text[:text.index(old)].count("\n") + 1
            source.write_text(text.replace(old, new, 1))
            executable = directory / "contract-test"
            try:
                diff.build(directory, executable)
                if sweep or name in equivalent_deletions:
                    cs, co, cv = contract(executable)
                    ds, do, dv = full_differential(executable, directory, corpus)
                    if not cv or not dv or cs not in (0, 1) or ds not in (0, 1):
                        raise RuntimeError(f"invalid suite run: contract={cs}, differential={ds}\n{co}\n{do}")
                    if name in equivalent_deletions:
                        equivalent = cs == 0 and ds == 0
                        print(f"{name} | {line} | contract={cs} differential={ds} | "
                              f"{'SURVIVED (cannot change an answer)' if equivalent else 'EQUIVALENCE DISPROVED'}: "
                              f"{equivalent_deletions[name]}", flush=True)
                        if not equivalent: failed = True
                        continue
                    if name in contract_guards:
                        intended = re.search(
                            rf"^evidence {re.escape(case)} FAIL ({re.escape(field)}(?:[.\[].*)?)$", co, re.M)
                        killed = cs == 1 and intended
                    else:
                        intended = re.search(
                            rf"^evidence differential {re.escape(case)} MISMATCH ({re.escape(field)}) ", do, re.M)
                        killed = ds == 1 and intended
                    print(f"{name} | {line} | {'FAIL(assertion)' if cs else 'PASS'} | "
                          f"{'FAIL(assertion)' if ds else 'PASS'} | {case}:{field} | "
                          f"{'KILLED' if killed else 'SURVIVED'}", flush=True)
                    if not killed: failed = True
                    continue
                if name in differential_names:
                    status, output, valid = differential_case(executable, directory, case)
                    intended = re.search(
                        rf"^evidence differential {re.escape(case)} MISMATCH ({re.escape(field)}) ",
                        output, re.M)
                else:
                    status, output, valid = contract(executable)
                    intended = re.search(
                        rf"^evidence {re.escape(case)} FAIL ({re.escape(field)}(?:[.\[].*)?)$",
                        output, re.M)
            except (RuntimeError, OSError) as error:
                print(f"{name} SURVIVED/ERROR (build/run failure is not a kill): {error}", flush=True)
                failed = True
                continue
            if status == 1 and valid and intended:
                extra = "; both result objects unchanged" if name == "E8_shift_coverage" else ""
                print(f"{name} KILLED ({case}, {intended[1]}, build OK{extra})", flush=True)
            else:
                print(f"{name} SURVIVED/ERROR (exit {status}; intended comparison not proven)\n{output}", flush=True)
                failed = True
        label = "evidence guard deletions" if sweep else "evidence mutants"
        print(f"{label}: {len(all_mutants)} checked, " + ("FAIL" if failed else f"{len(all_mutants)-len(equivalent_deletions)} KILLED; {len(equivalent_deletions)} equivalent survivors (cannot change an answer)"))
        return 1 if failed else 0


try:
    sys.exit(main())
except Exception as error:
    print(f"evidence mutants: harness error (not a kill): {error}", file=sys.stderr)
    sys.exit(2)
PY
