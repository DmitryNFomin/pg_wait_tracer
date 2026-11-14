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
     [('if (r.seq != t->seq[k] + 1 || t->seq[k] == UINT64_MAX)', 'if (false)')]),
    ('per_record_write', 'dump', 'src/merge_capture.c',
     [('return data || pgwt_capture_dump_flush(dump);', 'return pgwt_capture_dump_flush(dump);')]),
    ('positive_ring_limit', 'bounded_ring', 'src/percpu_transport.c',
     [('return limit && *consumed >= limit ? -EAGAIN : 0;', 'return limit && *consumed >= limit ? 1 : 0;')]),
    ('skip_shutdown_drain', 'shutdown_raw', 'src/percpu_transport.c',
     [('for (unsigned pass = 0; pass < 256; ++pass) {\n        int drained = t->io.drain(t->io.ctx);', 'for (unsigned pass = 0; pass < 256; ++pass) {\n        int drained = 1;')]),
    ('forget_historical_proof', 'lifetime_history', 'src/merge_capture.c',
     [('if (!c->lifetime_index[slot]) {',
       'if (c->lifetime_index[slot]) c->lifetimes[c->lifetime_index[slot]-1].proof = MA_NOT_ATTEMPTED;\n        if (!c->lifetime_index[slot]) {')]),
    ('per_record_wakeup', 'cadence', 'src/percpu_transport.c',
     [('.sample_period = 1, .watermark = true,', '.sample_period = 1, .watermark = false, .wakeup_events = 1,')]),
    ('per_pass_topology', 'cadence', 'src/percpu_transport.c',
     [('if (!pgwt_transport_service_due(now, t->last_service_at, t->serviced))',
       'if (false && !pgwt_transport_service_due(now, t->last_service_at, t->serviced))')]),
    ('slot_gated_completion', 'completion_retry', 'src/percpu_transport.c',
     [('if (!pgwt_transport_service_due(now, t->last_service_at, t->serviced))\n        return complete_round(t);',
       'if (!pgwt_transport_service_due(now, t->last_service_at, t->serviced))\n        return 0;')]),
    ('skip_terminal_attempts', 'terminal_attempts', 'src/percpu_transport.c',
     [('if (done && !t->suspended) {', 'if (false && done && !t->suspended) {')]),
    ('skip_tick_entry', 'pending', 'src/merge_capture.c',
     [('    pgwt_tick_duties(d);', '    if (0) pgwt_tick_duties(d);')]),
    ('omit_core_backlog', 'core_held_age', 'src/merge_capture.c',
     [('return c ? c->retained_count + c->staged : 0;', 'return c ? c->retained_count : 0;')]),
    ('omit_adapter_backlog', 'pending', 'src/merge_capture.c',
     [('return c ? c->retained_count + c->staged : 0;', 'return c ? c->staged : 0;')]),
]
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
                            'perf cadence:', 'poll budget:', 'completion retry:', 'terminal attempts:')):
            print(line)
    for name, guard, path, edits in mutations:
        original = (work / path).read_text()
        changed = original
        for before, after in edits:
            if changed.count(before) != 1:
                raise SystemExit(f'ERROR: {name}: mutation anchor is not unique')
            changed = changed.replace(before, after)
        (work / path).write_text(changed)
        build()
        p = subprocess.run([str(binary), '--guard', guard],
                           cwd=repo, capture_output=True, text=True, env=env)
        (work / path).write_text(original)
        if (p.returncode != 1 or f'REACHED {guard}\n' not in p.stdout or
                f'transport ASSERT {guard}:' not in p.stderr or
                any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at {guard}')
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
    start = transport.index('static int consume_all(')
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
            any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at tick_duties')
    print(f'transport + tick/publication/drain/cadence mutants: {len(mutations)+7}/{len(mutations)+7} killed at intended assertions')
