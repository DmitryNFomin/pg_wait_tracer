#!/usr/bin/env python3
"""Production admission and completion mutants: a build failure is not a kill."""
import hashlib
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
mutations = [
    ('service_without_clock', 'service_deadline', 'src/percpu_transport.c',
     [('if (pgwt_transport_pass_expired(t)) return 1; /* retain the suffix */',
       'if (false) return 1; /* retain the suffix */')]),
    ('merge_without_clock', 'merge_deadline', 'src/merge_capture.c',
     [('if (c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) return 0;',
       'if (c->budget.active && c->budget.until == c->budget.started) return 0;')]),
    ('finish_ignores_deadline', 'finish_deadline', 'src/percpu_transport.c',
     [('if (now < started || now >= until) break;', 'if (false && (now < started || now >= until)) break;'),
      ('pgwt_transport_begin_pass(t, until);', 'pgwt_transport_begin_pass(t, 0);')]),
    ('finish_counter_dropped', 'finish_deadline', 'src/percpu_transport.c',
     [('++t->stats.finish_deadline_reached;', '/* omitted finish deadline counter */')]),
    ('synthetic_daemon', 'admission', 'src/merge_capture.c',
     [('.synthetic_premises = false', '.synthetic_premises = true')]),
    ('skip_cpu_ack', 'missing_ack', 'src/percpu_transport.c',
     [('i < t->ncpu; ++i) if (!t->acks[i])', 'i + 1 < t->ncpu; ++i) if (!t->acks[i])')]),
    ('early_counter_read', 'counter_timing', 'src/percpu_transport.c',
     [('for (size_t i = 0; i < t->ncpu; ++i) if (!t->acks[i]) return 0;',
       'counters(t);\n    for (size_t i = 0; i < t->ncpu; ++i) if (!t->acks[i]) return 0;'),
      ('/* Completion fence: this reread MUST follow every CPU\'s consumed ack. */\n    counters(t);',
       '/* Mutant uses the earlier counter observation. */')]),
    ('accept_stale_round', 'stale_ack', 'src/percpu_transport.c',
     [('r.round > t->round)', 'r.round >= t->round)'),
      ('r.round <= t->round :', 'r.round < t->round :')]),
    ('ignore_seq_gap', 'gaps', 'src/percpu_transport.c',
     [('    if (skipped) {\n        /* The numbers in between', '    if (false) {\n        /* The numbers in between')]),
    ('per_record_write', 'dump', 'src/merge_capture.c',
     [('return data || pgwt_capture_dump_flush(dump);', 'return pgwt_capture_dump_flush(dump);')]),
    ('positive_ring_limit', 'bounded_ring', 'src/percpu_transport.c',
     [('return limit && *consumed >= limit ? -EAGAIN : 0;', 'return limit && *consumed >= limit ? 1 : 0;')]),
    ('skip_shutdown_drain', 'shutdown_raw', 'src/percpu_transport.c',
     [('int drained = bounded_drain(t);', 'int drained = 1;')]),
    ('forget_historical_proof', 'lifetime_history', 'src/merge_capture.c',
     [('if (!c->lifetime_index[slot]) {',
       'if (c->lifetime_index[slot]) c->lifetimes[c->lifetime_index[slot]-1].proof = MA_NOT_ATTEMPTED;\n        if (!c->lifetime_index[slot]) {')]),
    ('per_record_wakeup', 'cadence', 'src/percpu_transport.c',
     [('.sample_period = 1, .watermark = true,', '.sample_period = 1, .watermark = false, .wakeup_events = 1,')]),
    ('per_pass_topology', 'cadence', 'src/percpu_transport.c',
     [('if (!pgwt_transport_service_due(now, t->last_service_at, t->serviced))',
       'if (false && !pgwt_transport_service_due(now, t->last_service_at, t->serviced))')]),
    ('slot_gated_completion', 'never_empty', 'src/percpu_transport.c',
     [('        rc = fence_round(t);\n        if (rc < 0 || t->suspended) return rc;\n        return complete_round(t);\n    }\n    t->last_service_at = now;',
       '        return 0;\n    }\n    t->last_service_at = now;')]),
    # The completion rule (issue 871, both advisers and the lead).
    ('restore_empty_gate', 'never_empty', 'src/percpu_transport.c',
     [('    /* After every ack: a failed attempt of this round\'s window is counted',
       '    if (!t->pass_empty) return 0;\n    /* After every ack: a failed attempt of this round\'s window is counted')]),
    ('constant_el_token', 'never_empty', 'src/percpu_transport.c',
     [('.el_request = fence ? 0 : t->el_token};', '.el_request = fence ? 0 : 1};')]),
    ('moving_target', 'snapshot_hold', 'src/percpu_transport.c',
     [('    bool short_e = false, short_l = false;',
       '    if (t->io.attempts(t->io.ctx, t->targets, t->ncpu * 3) < 0) return fault(t, PGWT_MERGE_COUNTER_UNKNOWN);\n    bool short_e = false, short_l = false;')]),
    ('moving_cut', 'snapshot_hold', 'src/percpu_transport.c',
     [('        if (!t->cut_taken) {\n            t->cut_taken = true; t->cut_passes = 0;',
       '        if (true) {\n            t->cut_taken = true;')]),
    ('diagnose_before_cut', 'snapshot_hold', 'src/percpu_transport.c',
     [('if ((short_e && consumed[0] < t->cut[0]) || (short_l && consumed[1] < t->cut[1])) {', 'if (false) {')]),
    ('ignore_failure_counters', 'terminal_failure', 'src/percpu_transport.c',
     [('    for (size_t k = 0; k < n; ++k)\n        if (t->failures[k]) return fault(t, PGWT_MERGE_SEQUENCE_GAP);\n', '')]),
    ('no_failure_poll_while_open', 'terminal_failure', 'src/percpu_transport.c',
     [('    if (t->requested) {\n        /* A failed boundary or acknowledgement is never delivered',
       '    if (false) {\n        /* A failed boundary or acknowledgement is never delivered')]),
    ('unreadable_accounting_is_zero', 'terminal_failure', 'src/percpu_transport.c',
     [('        t->io.discards(t->io.ctx, t->discards, n) < 0)\n        return fault(t, PGWT_MERGE_COUNTER_UNKNOWN);',
       '        t->io.discards(t->io.ctx, t->discards, n) < 0)\n        return 0;')]),
    ('uncounted_skip_accepted', 'discards', 'src/percpu_transport.c',
     [('        if (t->holes[k] > t->discards[k]) return fault(t, PGWT_MERGE_SEQUENCE_GAP);', '        (void)k;')]),
    ('repeat_accepted', 'discards', 'src/percpu_transport.c',
     [('    if (r.seq <= t->seq[k] || t->seq[k] == UINT64_MAX) {', '    if (false) {')]),
    ('ignore_terminal_hole', 'terminal_discard', 'src/percpu_transport.c',
     [('                return fault(t, PGWT_MERGE_SEQUENCE_GAP); /* terminal hole */', '                continue; /* terminal hole */')]),
    ('unbounded_cut_wait', 'terminal_discard', 'src/percpu_transport.c',
     [('            if (++t->cut_passes > t->cut_limit) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);', '            ++t->cut_passes;')]),
    ('terminal_discard_is_delivered', 'terminal_discard', 'src/percpu_transport.c',
     [('            t->stats.terminal_discards += missing;', '            t->stats.terminal_discards += missing; t->seq[k] = t->targets[k];')]),
    ('uncovered_into_merge', 'uncovered', 'src/merge_capture.c',
     [('    bool uncovered = e->kind != MA_TEXT && !(e->cookie_present && e->cookie);', '    bool uncovered = false;')]),
    ('skip_terminal_attempts', 'terminal_attempts', 'src/percpu_transport.c',
     [('if (done && !t->suspended) {', 'if (false && done && !t->suspended) {')]),
    ('skip_tick_entry', 'pending', 'src/merge_capture.c',
     [('    pgwt_tick_duties(d);', '    if (0) pgwt_tick_duties(d);')]),
    ('omit_core_backlog', 'core_held_age', 'src/merge_capture.c',
     [('return c ? c->retained_count + c->staged : 0;', 'return c ? c->retained_count : 0;')]),
    ('omit_adapter_backlog', 'pending', 'src/merge_capture.c',
     [('return c ? c->retained_count + c->staged : 0;', 'return c ? c->staged : 0;')]),
]
deadline_assertions = {
    'service_without_clock': 'd.now-started<=PGWT_TRANSPORT_PASS_NS',
    'merge_without_clock': 'd.now-started<=PGWT_TRANSPORT_PASS_NS',
    'finish_ignores_deadline': 'd.now-started<=PGWT_TRANSPORT_FINISH_NS',
    'finish_counter_dropped': 'pgwt_transport_stats(d.transport).finish_deadline_reached==1',
}
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DPGWT_TRANSPORT_HOST_TEST']
flags += shlex.split(os.environ.get('CFLAGS', ''))
cc = shlex.split(os.environ.get('CC', 'cc'))
env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:exitcode=86',
           UBSAN_OPTIONS='halt_on_error=1:exitcode=87:print_stacktrace=1')
with tempfile.TemporaryDirectory(prefix='pgwt-percpu-mutants-') as temp:
    work = Path(temp)
    shutil.copytree(repo / 'src', work / 'src')
    (work / 'tests').mkdir()
    for name in ['test_percpu_transport.c', 'test_core_merge_adapter.c',
                 'test_core_merge_contract.c', 'merge_adapter_goldens.h']:
        shutil.copy2(repo / 'tests' / name, work / 'tests' / name)
    sources = ['tests/test_percpu_transport.c', 'src/percpu_transport.c',
               'src/merge_capture.c', 'src/core/merge_adapter.c', 'src/core/merge.c']
    binary = work / 'transport-test'
    def build():
        p = subprocess.run(cc + flags + sources + ['-o', str(binary)],
                           cwd=work, capture_output=True, text=True, env=env)
        if p.returncode:
            raise SystemExit('ERROR: mutant compilation is not a kill\n' + p.stdout + p.stderr)
    build()
    p = subprocess.run([str(binary), str(repo / 'contracts/merge-v1/examples.json')],
                       cwd=repo, capture_output=True, text=True, env=env)
    if p.returncode or 'production wire rows 1-43: 43/43 N PASS' not in p.stdout:
        raise SystemExit('ERROR: baseline failed\n' + p.stdout + p.stderr)
    summary = re.search(r'production wire rows 1-43: 43/43 N PASS; completion/refusal guards: (\d+)/\1 PASS', p.stdout)
    if not summary:
        raise SystemExit('ERROR: guard sweep summary missing')
    print('transport baseline: ' + summary.group(0))
    for line in p.stdout.splitlines():
        if line.startswith(('transport all_acks ', 'transport missing_ack ', 'transport stale_ack ',
                            'transport counter_timing ', 'transport post_snapshot ', 'shutdown:',
                            'binary sink:', 'counter cost:', 'indexed lookups ', 'lifetime history:',
                            'perf cadence:', 'poll budget:', 'completion:', 'completion control:',
                            'discards:', 'terminal hole:', 'uncovered:', 'terminal attempts:')):
            print(line)
    for name, guard, path, edits in mutations:
        original = (work / path).read_text()
        changed = original
        for before, after in edits:
            expected = 2 if name == 'finish_ignores_deadline' and before.startswith('if (now') else 1
            if changed.count(before) != expected:
                raise SystemExit(f'ERROR: {name}: mutation anchor is not unique')
            changed = changed.replace(before, after)
        (work / path).write_text(changed)
        build()
        p = subprocess.run([str(binary), '--guard', guard],
                           cwd=repo, capture_output=True, text=True, env=env)
        (work / path).write_text(original)
        if (p.returncode != 1 or f'REACHED {guard}\n' not in p.stdout or
                f'transport ASSERT {guard}:' not in p.stderr or
                (name in deadline_assertions and deadline_assertions[name] not in p.stderr) or
                any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at {guard}' +
              (f' ({deadline_assertions[name]})' if name in deadline_assertions else ''))
    print(f'transport mutants: {len(mutations)}/{len(mutations)} killed at intended assertions')

    # The daemon TU requires Linux/BPF loader headers. Compile ONLY the real
    # factored C function and real publication observer against portable
    # side-effect stubs; their bodies are read verbatim, not reimplemented.
    daemon = (repo / 'src/daemon.c').read_text()
    start = daemon.index('void pgwt_tick_duties(')
    end = daemon.index('\nstatic void start_sampler_sched', start)
    tick = daemon[start:end]
    capture = (repo / 'src/merge_capture.c').read_text()
    start = capture.index('static int daemon_observe(')
    end = capture.index('static bool daemon_write(', start)
    observe = capture[start:end]
    normalized = tick[tick.index('    /* Health check:'):]
    # Match only for the migration comparison. Mutants below are compiled
    # and must fail behavior; these anchors do not classify a mutant kill.
    drain_start = normalized.index('    if (d->exact_perf) {')
    drain_end = normalized.index('\n\n    /* Copy cumulative', drain_start)
    transport_drain = normalized[drain_start:drain_end]
    if normalized.count(transport_drain) != 1:
        raise SystemExit('ERROR: shared tick drain dispatch changed')
    normalized = normalized.replace(transport_drain, '    drain_event_ring_for_tick(d);')
    normalized = normalized.replace(
        '''    /* #416: the LAST thing before the display is built. The default ring
     * drains into event_accum; exact-perf can still hold records for proof
     * or a frontier. Report that backlog without changing release order. */''',
        '''    /* #416: the LAST thing before the display is built, so the smallest
     * possible window separates "every emitted record is in event_accum"
     * from the state_map read that supplies the still-open stretches. */''')
    # Migration check of every original statement/comment/log line, fe519c9e
    # daemon.c:443-677. This is behavior compatibility, not a numerical oracle.
    if hashlib.sha256(normalized.encode()).hexdigest() != '93f3ad6f6ce781839a162ef8e3459845bfc9f88a2152e35a00e3be2efb0dfff3':
        raise SystemExit('ERROR: default tick body changed beyond the shared drain dispatch')
    if 'rodata->exact_perf_mode' in (repo / 'src/percpu_transport.c').read_text():
        raise SystemExit('ERROR: tautological rodata support check returned')
    print('default tick: fe519c9e lines 443-677 byte-identical after substituting default drain; order/logs PASS')
    transport = (repo / 'src/percpu_transport.c').read_text()
    start = transport.index('static int receive_ring(')
    end = transport.index('static int linux_drain(', start)
    drain = transport[start:end]
    start = transport.index('int pgwt_transport_ring_result(')
    end = transport.index('int pgwt_transport_perf(', start)
    ring_result = transport[start:end]
    start = capture.index('int pgwt_merge_capture_tick(')
    end = capture.index('uint64_t pgwt_merge_capture_pending_records(', start)
    tick_entry = capture[start:end]
    start = capture.index('int pgwt_merge_capture_poll(')
    end = capture.index('int pgwt_merge_capture_tick(', start)
    capture_poll = capture[start:end].replace('int pgwt_merge_capture_poll(', 'static int capture_poll_under_test(')
    start = transport.index('uint64_t pgwt_transport_service_interval_ns(')
    end = transport.index('static uint64_t read_le(', start)
    cadence = transport[start:end]
    harness = work / 'tick.c'
    tick_binary = work / 'tick-test'
    def tick_build(tick_body, observer, drain_body=drain, poll_body=capture_poll):
        harness.write_text('#include "percpu_tick_harness.h"\n' + tick_body + observer + ring_result + drain_body + tick_entry + cadence + poll_body)
        p = subprocess.run(cc + flags + ['-I' + str(repo / 'tests'), '-I' + str(repo / 'src'),
            str(harness), '-o', str(tick_binary)], capture_output=True, text=True, env=env)
        if p.returncode:
            raise SystemExit('ERROR: tick/observer compile failure is not a kill\n' + p.stdout + p.stderr)
    tick_build(tick, observe)
    p = subprocess.run([str(tick_binary)], capture_output=True, text=True, env=env)
    if p.returncode:
        raise SystemExit('ERROR: shared tick/publication baseline failed\n' + p.stdout + p.stderr)
    if p.stderr.count('WARN: exact-perf per-tick drain left ') != 6:
        raise SystemExit('ERROR: tick backlog warning must be one-shot per daemon\n' + p.stderr)
    print(p.stdout, end='')
    full_observe = observe.replace(
        'o->counters.value[PGWT_C_WRITER_PUSH_FAIL] = d->counters.writer_push_fail_total;',
        '''ma_snapshot s; uint64_t now;
    if (pgwt_merge_capture_snapshot(d, &s, &now) < 0) return -1;
    o->counters.known = s.valid;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) o->counters.value[i] = s.value[i];''')
    for name, body, observer, drain_body, poll_body in [
        ('skip_tick_count', tick.replace('d->tick++;', '/* skip tick */'), observe, drain, capture_poll),
        ('per_record_map_reads', tick, full_observe, drain, capture_poll),
        ('ignore_perf_receipts', tick, observe, drain.replace('if (l->d->percpu_transport->positions[i] != before)', 'if (n || before > l->d->percpu_transport->positions[i])'), capture_poll),
        ('perf_without_clock', tick, observe, drain.replace(
            'return pgwt_transport_pass_expired(l->d->percpu_transport) ? LIBBPF_PERF_EVENT_DONE : LIBBPF_PERF_EVENT_CONT;',
            '(void)l; return LIBBPF_PERF_EVENT_CONT;'), capture_poll),
        ('ring_without_clock', tick, observe, drain.replace(
            'if (!rc && pgwt_transport_pass_expired(r->d->percpu_transport)) return -EINTR;',
            'if (false) return -EINTR;'), capture_poll),
        ('continue_failed_tick_drain', tick.replace(
            'd->running = false;\n            return;\n        }\n        d->counters.tick_drain_pending_records',
            '/* continue after failure */\n            return;\n        }\n        d->counters.tick_drain_pending_records'), observe, drain, capture_poll),
        ('omit_tick_backlog', tick.replace(
            'd->counters.tick_drain_pending_records = pgwt_merge_capture_pending_records(d);',
            'd->counters.tick_drain_pending_records = 0;'), observe, drain, capture_poll),
        ('per_pass_core_checks', tick, observe, drain, capture_poll.replace(
            'if (rc >= 0 && !pgwt_transport_service_due(',
            'if (false && rc >= 0 && !pgwt_transport_service_due(')),
        ('repeat_backlog_warning', tick.replace('if (!d->tick_drain_truncated_logged)',
                                                'if (true || !d->tick_drain_truncated_logged)'), observe, drain, capture_poll),
    ]:
        if body == tick and observer == observe and drain_body == drain and poll_body == capture_poll:
            raise SystemExit('ERROR: missing tick/publication mutant anchor')
        tick_build(body, observer, drain_body, poll_body)
        p = subprocess.run([str(tick_binary)], capture_output=True, text=True, env=env)
        if (p.returncode != 1 or 'REACHED tick_duties\n' not in p.stdout or
            'transport ASSERT tick_duties:' not in p.stderr or
            (name == 'perf_without_clock' and
             'service_now<=PGWT_TRANSPORT_PASS_NS && cpu0.records==15' not in p.stderr) or
            (name == 'ring_without_clock' and
             'service_now-started<=PGWT_TRANSPORT_PASS_NS && e.records==15' not in p.stderr) or
            any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at tick_duties')
    print(f'transport + tick/publication/drain/cadence mutants: {len(mutations)+9}/{len(mutations)+9} killed at intended assertions')
