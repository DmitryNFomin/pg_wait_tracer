#!/usr/bin/env python3
"""Production admission and completion mutants: a build failure is not a kill."""
import hashlib
import ast
import contextlib
import io
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

repo = Path(__file__).resolve().parent.parent

def deadline_gate_cases(source):
    """Run the live arm's real orchestration with a virtual clock/processes.
    No daemon, PostgreSQL, BPF, wall-clock waits or sleeps are involved."""
    node = next(n for n in ast.parse(source).body
                if isinstance(n, ast.FunctionDef) and n.name == 'run_deadline_arm')
    code = compile(ast.Module(body=[node], type_ignores=[]), '<live deadline arm>', 'exec')
    for name, terminate, stop_at, expected in [
        ('early_zero_exit', False, 11.0, 'duration: stayed alive for the requested duration'),
        ('whole_second_deadline', False, 28.0, None),
        ('full_duration', False, 29.0, None),
        ('duration_overrun', False, 45.0, 'duration: capture exit deadline exceeded'),
        ('sigterm', True, 2.0, None),
        ('sigterm_overrun', True, 16.0, 'sigterm: capture exit deadline exceeded'),
    ]:
        clock = SimpleNamespace(now=10.75)
        processes = []
        class Process:
            def __init__(self, command, **kwargs):
                self.daemon = not processes
                self.returncode = None
                self.signaled = False
                self.exit_at = stop_at if not terminate else float('inf')
                processes.append(self)
            def poll(self):
                return self.returncode
            def wait(self, timeout):
                if self.returncode is None:
                    if self.daemon and clock.now + timeout < self.exit_at:
                        clock.now += timeout
                        raise subprocess.TimeoutExpired('virtual daemon', timeout)
                    clock.now = max(clock.now, self.exit_at) if self.daemon else clock.now
                    self.returncode = 0
                return self.returncode
            def send_signal(self, sig):
                self.signaled = True
                self.exit_at = clock.now + stop_at
            def terminate(self):
                self.returncode = 0
            def kill(self):
                self.returncode = -9
        def ready(*args):
            clock.now = 11.0
        namespace = dict(os=os, time=SimpleNamespace(monotonic=lambda: clock.now),
                         subprocess=SimpleNamespace(Popen=Process, DEVNULL=subprocess.DEVNULL,
                                                    TimeoutExpired=subprocess.TimeoutExpired),
                         TRACER=repo / 'pg_wait_tracer', wait_for=ready,
                         signal=SimpleNamespace(SIGTERM=15),
                         measurements=lambda text: ({'status': 0}, {}),
                         qualification=lambda path: {'end': {'status': 0}, 'voids': []})
        exec(code, namespace)
        error = None
        with tempfile.TemporaryDirectory(prefix='pgwt-deadline-host-') as temp:
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    namespace['run_deadline_arm'](1, 'virtual', Path(temp), False, 18, terminate)
            except AssertionError as caught:
                error = str(caught)
        if expected is None:
            assert error is None, (name, error)
        else:
            assert error and error.startswith(expected), f'{name}: expected rejection at {expected}; got {error}'
        assert len(processes) == 2 and all(p.returncode is not None for p in processes), name
        if terminate:
            assert processes[0].signaled, name
    return 'live deadline orchestration: 6/6 virtual process cases PASS (early zero exit rejected)'

mutations = [
    ('release_sleeps', 'service_deadline', 'src/percpu_transport.c',
     [('pgwt_budget_yield(&t->budget);\n            ++t->stats.release_budget_yields;',
       '++t->stats.release_budget_yields;')]),
    ('blanket_finish_void', 'rv_uncovered_finish', 'src/percpu_transport.c',
     [('bool releasing;', 'bool releasing, finishing;'),
      ('bool done = false;\n    unsigned pass;', 't->finishing = true;\n    bool done = false;\n    unsigned pass;'),
      ('    if (t->io.received) {',
       '    if (t->finishing && r.envelope.kind != MA_BOUNDARY) {\n'
       '        ++t->stats.unfinished_records;\n        return 0;\n    }\n    if (t->io.received) {'),
      ('    /* Terminal reconciliation, per (CPU,destination), after an empty pass:',
       '    if (t->stats.unfinished_records && rc >= 0 && !t->suspended)\n'
       '        rc = fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);\n'
       '    /* Terminal reconciliation, per (CPU,destination), after an empty pass:')]),
    ('skip_final_budget_edge_check', 'rv_final_check', 'src/merge_capture.c',
     [('    ma_snapshot current = s ? *s : (ma_snapshot){0};\n    bool hold;\n'
       '    int rc = mapped(c, &check, d, &current, &hold);',
       '    ma_snapshot current = s ? *s : (ma_snapshot){0};\n    bool hold;\n    int rc = c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx)) ? 0 : mapped(c, &check, d, &current, &hold);')]),
    ('failures_as_unfinished', 'finish_failures', 'src/percpu_transport.c',
     [('        unread = unread > t->failures[k] ? unread - t->failures[k] : 0;\n', '')]),
    ('pass_cap_as_deadline', 'shutdown_raw', 'src/percpu_transport.c',
     [('if (pass_limit) ++t->stats.finish_pass_limit_reached;',
       'if (pass_limit) ++t->stats.finish_deadline_reached;')]),
    ('service_without_clock', 'service_deadline', 'src/percpu_transport.c',
     [('if (pgwt_transport_pass_expired(t)) {\n            pgwt_budget_yield(&t->budget);\n            ++t->stats.release_budget_yields;',
       'if (false) {\n            ++t->stats.release_budget_yields;')]),
    ('merge_without_clock', 'merge_deadline', 'src/merge_capture.c',
     [('if (c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) {\n            pgwt_budget_yield(&c->budget);\n            ++c->source_budget_yields;',
       'if (c->budget.active && c->budget.until == c->budget.started) {\n            ++c->source_budget_yields;')]),
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
     [('if (++t->cut_passes > t->cut_limit) {', 'if (false && ++t->cut_passes > t->cut_limit) {')]),
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
# Each observation-bearing stop has its own skip mutant and assertion.
mutations += [
    ('skip_fence_edge_check', 'rv_fence_edge', 'src/merge_capture.c',
     [('    /* Control inputs carry observations:',
       '    if (e->kind == MA_CHECK && c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) return 0;\n    /* Control inputs carry observations:')]),
    ('skip_periodic_edge_check', 'periodic_edge', 'src/merge_capture.c',
     [('        int rc = mapped(c, e, d, &current, &hold);',
       '        int rc = e->kind == MA_CHECK && c->budget.active && d >= c->budget.until ? 0 : mapped(c, e, d, &current, &hold);')]),
    ('skip_source_edge_observation', 'source_edge', 'src/merge_capture.c',
     [('            return mapped(c, &check, d, s, &hold);',
       '            (void)check; (void)hold; return 0;')]),
    ('skip_sweep_edge_check', 'sweep_edge', 'src/merge_capture.c',
     [('        int rc = mapped(c, e, d, &current, &hold);',
       '        int rc = c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx)) ? 0 : mapped(c, e, d, &current, &hold);')]),
    ('skip_release_resume_check', 'release_edge', 'src/merge_capture.c',
     [('    /* Control inputs carry observations:',
       '    if (e->kind == MA_CHECK && c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) return 0;\n    /* Control inputs carry observations:')]),
    ('skip_finish_entry_check', 'finish_entry_check', 'src/percpu_transport.c',
     [('        rc = t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {',
       '        rc = timed_out ? 0 : t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {')]),
    ('skip_finish_time_check', 'finish_time_check', 'src/percpu_transport.c',
     [('        rc = t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {',
       '        rc = timed_out ? 0 : t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {')]),
    ('skip_finish_cap_check', 'finish_cap_check', 'src/percpu_transport.c',
     [('        rc = t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {',
       '        rc = pass_limit ? 0 : t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);\n        if (rc >= 0) {')]),
    ('skip_cut_cap_check', 'cut_cap_check', 'src/percpu_transport.c',
     [('                rc = t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot);',
       '                (void)check; rc = 0;')]),
]
deadline_assertions = {
    'release_sleeps': 'daemon->service_backlog',
    'blanket_finish_void': 'pgwt_capture_report(d.capture).status==PGWT_MERGE_OK',
    'skip_final_budget_edge_check': 'd.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE',
    'failures_as_unfinished': 'stats.unfinished_records==unread',
    'pass_cap_as_deadline': 'pgwt_transport_stats(d.transport).finish_pass_limit_reached==1',
    'service_without_clock': 'd.now-started<=PGWT_TRANSPORT_PASS_NS',
    'merge_without_clock': 'd.now-started<=PGWT_TRANSPORT_PASS_NS',
    'finish_ignores_deadline': 'd.now-started<=PGWT_TRANSPORT_FINISH_NS',
    'finish_counter_dropped': 'pgwt_transport_stats(d.transport).finish_deadline_reached==1',
}
for name, guard, _, _ in mutations:
    if name.startswith('skip_') and guard in {'rv_fence_edge','periodic_edge','source_edge',
                                             'sweep_edge','release_edge','finish_entry_check','finish_time_check',
                                             'finish_cap_check','cut_cap_check'}:
        deadline_assertions[name] = 'd->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE'
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
        elif line.startswith(('REACHED rv_', 'RV-', 'finish accounting:', 'finish deadline:')):
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
    clock_body = capture[capture.index('static uint64_t daemon_now('):capture.index('static bool daemon_flush(')]
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
    start = transport.index('static int linux_consume_io(void *ctx,')
    end = transport.index('static void linux_close_io(void *ctx)', start)
    consume = transport[start:end]
    start = daemon.index('int pgwt_consume_event_ring(')
    end = daemon.index('/* Consume one lifecycle batch', start)
    event_drain = daemon[start:end]
    start = capture.index('int pgwt_merge_capture_tick(')
    end = capture.index('uint64_t pgwt_merge_capture_pending_records(', start)
    tick_entry = capture[start:end]
    start = capture.index('int pgwt_merge_capture_poll(')
    end = capture.index('int pgwt_merge_capture_tick(', start)
    capture_poll = capture[start:end].replace('int pgwt_merge_capture_poll(', 'static int capture_poll_under_test(')
    start = transport.index('uint64_t pgwt_transport_service_interval_ns(')
    end = transport.index('static uint64_t read_le(', start)
    cadence = transport[start:end]
    start = transport.index('void pgwt_transport_begin_pass(')
    end = transport.index('bool pgwt_transport_pass_expired(', start)
    begin_pass = transport[start:end]
    start = transport.index('static int bounded_drain(')
    end = transport.index('static int counters(', start)
    budget_drain = transport[start:end]
    start = transport.index('void pgwt_percpu_transport_begin_pass(')
    end = transport.index('int pgwt_percpu_transport_snapshot(', start)
    daemon_begin = transport[start:end]
    start = capture.index('static int flush_source(')
    end = capture.index('int pgwt_capture_attach(', start)
    capture_deferral = capture[start:end]
    # Pin the main-loop use/reset/preservation of the signal tested below.
    for statement in ['int timeout_ms = d->service_backlog ? 0 : poll_ms;',
                      'd->service_backlog = false;\n        pgwt_percpu_transport_begin_pass(',
                      'if (consume_event_ring(d, "event_ring_drain", true))\n                d->service_backlog = true;']:
        if statement not in daemon:
            raise SystemExit('ERROR: main loop no-sleep signal disconnected: ' + statement)
    start = daemon.index('        /* Drain event ringbuf on every iteration')
    end = daemon.index('        if (d->debug_dump_state)', start)
    main_drain = 'static void main_drain_under_test(struct pgwt_daemon *d) {\n' + daemon[start:end] + '}\n'
    timeout_expression = re.search(r'int timeout_ms = ([^;]+);', daemon).group(1)
    main_timeout = 'static int main_timeout_under_test(struct pgwt_daemon *d,int poll_ms) { return ' + timeout_expression + '; }\n'
    harness = work / 'tick.c'
    tick_binary = work / 'tick-test'
    def tick_build(tick_body, observer, drain_body=drain, poll_body=capture_poll,
                   consume_body=consume, event_body=event_drain,
                   budget_body=budget_drain, deferral_body=capture_deferral, main_body=main_drain):
        instrumented = observer.replace('static int daemon_observe(', 'static int daemon_observe_under_test(')
        instrumented += 'static int daemon_observe(void *ctx, pgwt_merge_observation *o) { ++observation_reads; return daemon_observe_under_test(ctx, o); }\n'
        harness.write_text('#include "percpu_tick_harness.h"\n' + tick_body + instrumented + clock_body + ring_result +
                           drain_body + consume_body + event_body + tick_entry + cadence + poll_body +
                           begin_pass + daemon_begin + budget_body + deferral_body + main_body + main_timeout)
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
            'if (pgwt_transport_pass_expired(l->d->percpu_transport)) {\n        pgwt_budget_yield(&l->d->percpu_transport->budget);\n        ++l->d->percpu_transport->stats.perf_budget_yields;',
            'if (false) {\n        pgwt_budget_yield(&l->d->percpu_transport->budget);\n        ++l->d->percpu_transport->stats.perf_budget_yields;'), capture_poll),
        ('ring_without_clock', tick, observe, drain.replace(
            'if (!rc && pgwt_transport_pass_expired(r->d->percpu_transport)) {',
            'if (false) {'), capture_poll),
        ('continue_failed_tick_drain', tick.replace(
            'd->running = false;\n            return;\n        }\n        d->counters.tick_drain_pending_records',
            '/* continue after failure */\n            return;\n        }\n        d->counters.tick_drain_pending_records'), observe, drain, capture_poll),
        ('omit_tick_backlog', tick.replace(
            'd->counters.tick_drain_pending_records = pgwt_merge_capture_pending_records(d);',
            'd->counters.tick_drain_pending_records = 0;'), observe, drain, capture_poll),
        ('per_pass_core_checks', tick, observe, drain, capture_poll.replace(
            'if (now && !pgwt_transport_service_due(',
            'if (false && now && !pgwt_transport_service_due(')),
        ('cadence_reads_before_gate', tick, observe, drain, capture_poll.replace(
            'uint64_t now = daemon_now(d);',
            'pgwt_merge_observation skipped = d->merge_capture->observation; (void)daemon_observe(d, &skipped);\n    uint64_t now = daemon_now(d);')),
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
            (name == 'cadence_reads_before_gate' and 'observation_reads==1' not in p.stderr) or
            any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at tick_duties' +
              (' (observation_reads==1)' if name == 'cadence_reads_before_gate' else ''))
    for name, consume_body, event_body, assertion in [
        ('expired_consume_sleeps', consume.replace('return dest == PGWT_WIRE_DEST_E ? -EAGAIN : 0;',
                                                  'return 0;'), event_drain,
         'consume_event_ring(d,"test",true) && e.records==15'),
        ('yielded_consume_sleeps', consume.replace('if (dest == PGWT_WIRE_DEST_E && yielded) return -EAGAIN;',
                                                  'if (dest == PGWT_WIRE_DEST_E && yielded) return rc;'), event_drain,
         'consume_event_ring(d,"test",true) && e.records==10'),
        ('daemon_forgets_yield', consume, event_drain.replace('d->exact_perf ? rc == -EAGAIN :',
                                                            'd->exact_perf ? false :'),
         'consume_event_ring(d,"test",true) && e.records==15'),
    ]:
        if consume_body == consume and event_body == event_drain:
            raise SystemExit('ERROR: missing yield mutant anchor')
        tick_build(tick, observe, consume_body=consume_body, event_body=event_body)
        p = subprocess.run([str(tick_binary)], capture_output=True, text=True, env=env)
        if (p.returncode != 1 or 'REACHED tick_duties\n' not in p.stdout or
                f'transport ASSERT tick_duties: {assertion}' not in p.stderr or
                any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at tick_duties ({assertion})')
    no_sleep_mutations = [
        ('perf', 'drain', 'pgwt_budget_yield(&l->d->percpu_transport->budget);\n        ++l->d->percpu_transport->stats.perf_budget_yields;',
         '++l->d->percpu_transport->stats.perf_budget_yields;', 'd->service_backlog && perf.records==1'),
        ('hint', 'drain', 'pgwt_budget_yield(&l->d->percpu_transport->budget);\n        ++l->d->percpu_transport->stats.hint_budget_yields;',
         '++l->d->percpu_transport->stats.hint_budget_yields;', 'd->service_backlog && hint.records==1'),
        ('reader', 'drain', 'pgwt_budget_yield(&l->d->percpu_transport->budget);\n            ++l->d->percpu_transport->stats.reader_budget_yields;',
         '++l->d->percpu_transport->stats.reader_budget_yields;', 'd->service_backlog && perf.records==1'),
        ('slice', 'budget', 'if (rc != 1) pgwt_budget_yield(&t->budget);',
         '(void)rc;', 'd->service_backlog && perf.records==1'),
        ('drain_entry', 'budget', 'pgwt_budget_yield(&t->budget);\n        ++t->stats.drain_budget_yields;',
         '++t->stats.drain_budget_yields;', 'd->service_backlog && perf.records==1'),
        ('E', 'drain', 'pgwt_budget_yield(&r->d->percpu_transport->budget);\n        ++r->d->percpu_transport->stats.ring_budget_yields;',
         '++r->d->percpu_transport->stats.ring_budget_yields;', 'd->service_backlog && e.records==1'),
        ('E_cap', 'drain', 'pgwt_budget_yield(&r->d->percpu_transport->budget);\n        ++r->d->percpu_transport->stats.ring_record_cap_yields;',
         '++r->d->percpu_transport->stats.ring_record_cap_yields;', 'd->service_backlog && e.records==904'),
        ('consume', 'consume', 'pgwt_budget_yield(&d->percpu_transport->budget);',
         '/* omitted signal */', 'd->service_backlog && e.records==904'),
        ('preserve', 'main', 'if (consume_event_ring(d, \"event_ring_drain\", true))\n                d->service_backlog = true;',
         'd->service_backlog = consume_event_ring(d, \"event_ring_drain\", true);', 'd->service_backlog'),
        ('capture_source', 'deferral', 'pgwt_budget_yield(&c->budget);\n            ++c->source_budget_yields;',
         '++c->source_budget_yields;', 'd->service_backlog && c.retained_count==1'),
        ('capture_sweep', 'deferral', 'if (c->retained_count) pgwt_budget_yield(&c->budget);',
         '/* omitted signal */', 'd->service_backlog && c.retained_count==1'),
    ]
    for kind, part, before, after, assertion in no_sleep_mutations:
        bodies = dict(drain=drain, budget=budget_drain, consume=consume, deferral=capture_deferral, main=main_drain)
        if bodies[part].count(before) != 1:
            raise SystemExit(f'ERROR: no_sleep_{kind}: missing unique anchor')
        bodies[part] = bodies[part].replace(before, after)
        tick_build(tick, observe, drain_body=bodies['drain'], budget_body=bodies['budget'],
                   consume_body=bodies['consume'], deferral_body=bodies['deferral'], main_body=bodies['main'])
        p = subprocess.run([str(tick_binary)], capture_output=True, text=True, env=env)
        if (p.returncode != 1 or 'REACHED tick_duties\n' not in p.stdout or
                f'transport ASSERT tick_duties: {assertion}' not in p.stderr or
                f'[no_sleep_{kind}]' not in p.stderr or
                any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: no_sleep_{kind}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'no_sleep_{kind}: KILLED at tick_duties ({assertion})')
    print(f'transport + tick/publication/drain/cadence/yield mutants: {len(mutations)+13+len(no_sleep_mutations)}/{len(mutations)+13+len(no_sleep_mutations)} killed at intended assertions')

    # Run each raw budget stop through the real transport, adapter and core.
    # The Linux callbacks are copied verbatim; only libbpf iteration is fake.
    shutil.copy2(repo / 'tests/percpu_budget_harness.h', work / 'tests/percpu_budget_harness.h')
    raw_binary = work / 'raw-test'
    raw_harness = work / 'tests/raw.c'
    def raw_build(body):
        callbacks = body[body.index('static int receive_ring('):body.index('static int linux_helpers_start(void *);')]
        consumer = body[body.index('static int linux_consume_io(void *ctx,'):body.index('static void linux_close_io(void *ctx)')]
        waiter = body[body.index('static int linux_wait(void *ctx)'):body.index('static int linux_envelope(', body.index('static int linux_wait(void *ctx)'))]
        base = (work / 'tests/test_percpu_transport.c').read_text().replace(
            'int main(int argc,char **argv)', 'int transport_original_main(int argc,char **argv)')
        # Avoid the fixture parser's unrelated static counters() helper.
        portable = re.sub(r'(?<![.\w])counters\(', 'transport_counters(', body)
        raw_harness.write_text(base + '\n' + portable + '\n#include "percpu_budget_harness.h"\n' + callbacks + consumer + waiter)
        p = subprocess.run(cc + flags + ['-I' + str(work / 'src'), str(raw_harness),
            'src/merge_capture.c', 'src/core/merge_adapter.c', 'src/core/merge.c', '-o', str(raw_binary)],
            cwd=work, capture_output=True, text=True, env=env)
        if p.returncode:
            raise SystemExit('ERROR: raw budget compilation is not a kill\n' + p.stdout + p.stderr)
    raw_build(transport)
    p = subprocess.run([str(raw_binary)], capture_output=True, text=True, env=env)
    if p.returncode or 'raw budget guards: 9/9 PASS' not in p.stdout:
        raise SystemExit('ERROR: raw budget baseline failed\n' + p.stdout + p.stderr)
    print(p.stdout, end='')
    raw_mutations = [
        ('drain_entry_false_empty', 'drain_entry_edge',
         '++t->stats.drain_budget_yields;\n        return 0;',
         '++t->stats.drain_budget_yields;\n        return 1;',
         'bounded_drain(d.transport)==0'),
        ('drain_slice_unbounded', 'drain_slice_edge',
         't->budget.until = now + PGWT_TRANSPORT_DRAIN_NS;',
         't->budget.until = service.until;', 'bounded_drain(d.transport)==0 && p0.left==1'),
        ('reader_false_empty', 'reader_edge',
         '++l->d->percpu_transport->stats.reader_budget_yields;\n            *empty = false;',
         '++l->d->percpu_transport->stats.reader_budget_yields;\n            *empty = true;',
         'consume_all(&l,&empty)==0 && !empty'),
        ('perf_unbounded', 'perf_edge',
         '++l->d->percpu_transport->stats.perf_budget_yields;\n        return LIBBPF_PERF_EVENT_DONE;',
         '++l->d->percpu_transport->stats.perf_budget_yields;\n        return LIBBPF_PERF_EVENT_CONT;',
         'consume_all(&l,&empty)==0 && !empty && p0.left==2'),
        ('hint_unbounded', 'hint_edge',
         '++l->d->percpu_transport->stats.hint_budget_yields;\n        return LIBBPF_PERF_EVENT_DONE;',
         '++l->d->percpu_transport->stats.hint_budget_yields;\n        return LIBBPF_PERF_EVENT_CONT;',
         'consume_all(&l,&empty)==0 && !empty && h0.left==2'),
        ('ring_unbounded', 'ring_edge',
         '++r->d->percpu_transport->stats.ring_budget_yields;\n        return -EINTR;',
         '++r->d->percpu_transport->stats.ring_budget_yields;\n        return 0;',
         'consume_all(&l,&empty)==0 && !empty && e.left==1'),
        ('consume_false_empty', 'consume_edge',
         'return dest == PGWT_WIRE_DEST_E ? -EAGAIN : 0;', 'return 0;',
         'linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==2'),
        ('wait_rounds_up_deadline', 'wait_edge',
         'epoll_wait(l->epoll_fd, events, 1, timeout);',
         'epoll_wait(l->epoll_fd, events, 1, 1);',
         'linux_wait(&l)==0 && raw_wait_timeout==0'),
        ('ring_cap_positive', 'ring_cap_edge',
         'return limit && *consumed >= limit ? -EAGAIN : 0;',
         'return limit && *consumed >= limit ? 1 : 0;',
         'linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==1'),
    ]
    # Raw stops carry no vector. Their resumption must still execute the
    # completion checkpoint. Mutate that hand-off separately for each stop;
    # each must reach VOID(counter_change), never DATA/FRONTIER/OK.
    fence = "/* Completion fence: this reread MUST follow every CPU's consumed ack. */\n    counters(t);"
    for guard, field in [
        ('drain_entry_edge', 'drain_budget_yields'), ('drain_slice_edge', 'drain_slice_yields'),
        ('reader_edge', 'reader_budget_yields'), ('perf_edge', 'perf_budget_yields'),
        ('hint_edge', 'hint_budget_yields'), ('ring_edge', 'ring_budget_yields'),
        ('consume_edge', 'consume_budget_yields'), ('ring_cap_edge', 'ring_record_cap_yields'),
        ('wait_edge', 'wait_budget_yields'),
    ]:
        raw_mutations.append(('skip_' + guard + '_checkpoint', guard, fence,
            fence.replace('counters(t);', f'if (!t->stats.{field}) counters(t);'),
            'd->outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE'))
    for name, guard, before, after, assertion in raw_mutations:
        if transport.count(before) != 1:
            raise SystemExit(f'ERROR: {name}: raw budget mutation anchor is not unique')
        raw_build(transport.replace(before, after))
        p = subprocess.run([str(raw_binary), '--guard', guard], capture_output=True, text=True, env=env)
        if (p.returncode != 1 or f'REACHED {guard}\n' not in p.stdout or
                f'transport ASSERT {guard}:' not in p.stderr or assertion not in p.stderr or
                any(s in p.stderr for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
            raise SystemExit(f'ERROR: {name}: survived or failed elsewhere\n' + p.stdout + p.stderr)
        print(f'{name}: KILLED at {guard} ({assertion})')
    print(f'raw budget mutants: {len(raw_mutations)}/{len(raw_mutations)} killed at intended assertions')

live_source = (repo / 'tests/test_transport_daemon_live.py').read_text()
print(deadline_gate_cases(live_source))
anchor = "assert exited >= int(launched) + duration, 'duration: stayed alive for the requested duration'"
assert live_source.count(anchor) == 1, 'live lower bound mutant anchor'
try:
    deadline_gate_cases(live_source.replace(anchor, "assert True, 'duration: stayed alive for the requested duration'"))
except AssertionError as error:
    assert str(error).startswith('early_zero_exit: expected rejection at duration: stayed alive'), str(error)
    print('natural_exit_without_lower_bound: KILLED at early_zero_exit')
else:
    raise SystemExit('ERROR: natural exit without lower bound survived')
print(f'transport host mutants total: {len(mutations)+len(raw_mutations)+len(no_sleep_mutations)+14}/{len(mutations)+len(raw_mutations)+len(no_sleep_mutations)+14} killed at intended assertions')
