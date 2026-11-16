#!/usr/bin/env python3
"""Compile real producer bodies against host shims; kill named built mutants.

Called by the registered C unit test (and make check). No BPF compilation/load.
"""
from pathlib import Path
import contextlib
import io
import json
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ('count_ring_fail', 'count_map_fail', 'test_fail_once',
             'transport_cookie', 'transport_attempt', 'transport_header',
             'transport_failure', 'transport_discard', 'transport_trace_failure', 'transport_lifecycle_failure',
             'transport_reserve_trace', 'transport_reserve_lifecycle', 'transport_spin_failure',
             'transport_boundary', 'on_transport_helper', 'emit_event',
             'on_watchpoint', 'on_bootstrap', 'on_fork', 'transport_exit', 'on_exit',
             'marker_edge', 'transport_marker', 'emit_marker', 'transport_text',
             'transport_activity', 'on_report_activity', 'on_report_query_id',
             'on_executor_start')


def function(source, name):
    matches = list(re.finditer(r'^(?:static(?: __always_inline)? )?(?:void|bool|u64|int|struct (?:pgwt_wire_(?:trace|lifecycle)|probe_identity)) \**' + name + r'\(', source, re.M))
    assert len(matches) == 1, (name, len(matches))
    start = matches[0].start()
    brace = source.index('{', matches[0].end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def replace_once(source, old, new):
    assert source.count(old) == 1, (old, source.count(old))
    return source.replace(old, new)


def restore_init_publication(source):
    """The same built mutant is exercised by the host and real-PG drivers.

    It puts the INIT publication on L back into on_bootstrap under the mode.
    It keeps the production reservation shape (reserve before the timestamp,
    one NULL test, the release inside the non-NULL arm), so the 4.18 verifier
    loads it and the only thing that differs from production is the record.

    On EL8 4.18 this reservation is taken in NMI context and is refused when
    the ring lock is contended, which is rare and load-dependent. So that
    every run of the mutant arm meets a refusal, on every kernel, the mutant
    also gives up the reservation for odd PIDs and counts it exactly as a
    refused one is counted (attempt consumed, nothing delivered, L failure,
    INIT site). The live arm must tolerate those and still reach its kill
    assertion; it asserts that it saw both a published and a refused INIT.
    """
    source = replace_once(source,
        '            if (st->init_stored) return 0;\n            u64 stamp = bpf_ktime_get_ns();',
        '            if (st->init_stored) return 0;\n            u64 seq;\n'
        '            struct pgwt_wire_lifecycle *wire = transport_reserve_lifecycle(&seq);\n'
        '            u64 stamp = bpf_ktime_get_ns();')
    return replace_once(source,
        '            bpf_perf_event_output(ctx, &init_wake, BPF_F_CURRENT_CPU, &pid, sizeof(pid));\n'
        '            return 0;\n        }\n'
        '        u64 init_ts = bpf_ktime_get_ns();',
        '''            bpf_perf_event_output(ctx, &init_wake, BPF_F_CURRENT_CPU, &pid, sizeof(pid));
            if (!wire) {
                transport_lifecycle_failure(PGWT_RING_FAIL_LIFECYCLE_INIT_HINT);
                return 0;
            }
            if (pid & 1) {
                bpf_ringbuf_discard(wire, 0);
                transport_lifecycle_failure(PGWT_RING_FAIL_LIFECYCLE_INIT_HINT);
                return 0;
            }
            transport_header(&wire->hdr, PGWT_WIRE_LIFECYCLE, sizeof(wire->payload),
                seq, st->cookie);
            wire->payload = (struct pgwt_lifecycle_event){ .type = PGWT_LIFECYCLE_INIT,
                .pid = pid, .addr = wait_addr, .timestamp = stamp };
            bpf_ringbuf_submit(wire, 0);
            return 0;
        }
        u64 init_ts = bpf_ktime_get_ns();''')


def run(source, directory, header=None):
    bodies = '\n\n'.join(function(source, name) for name in FUNCTIONS)
    harness = (ROOT / 'tests/transport_bpf_harness.h').read_text()
    unit = directory / 'producers.c'
    unit.write_text(harness.replace('/* PRODUCTION_FUNCTIONS */', bodies))
    binary = directory / 'producers'
    # A mutated helper is supplied by replacing its include in the test unit,
    # leaving production files untouched. Build failure never kills a mutant.
    if header:
        unit.write_text(unit.read_text().replace('#include "bpf/transport_producer.h"', header))
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra',
               '-Werror', '-Wno-unused-parameter', '-Wno-unused-variable',
               '-I', str(ROOT / 'src'), str(unit), '-o', str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    return subprocess.run([str(binary)], capture_output=True, text=True)


def run_probe_loop(source, directory):
    functions = ('probe_note_work', 'probe_close_backend', 'probe_identity',
                 'probe_new_backend', 'probe_instrument', 'probe_attach_uprobes', 'probe_close_uprobes',
                 'probe_record', 'probe_l',
                 'probe_teardown_one', 'probe_drain',
                 'probe_service_backends', 'probe_read_stats', 'probe_print_stats')
    stats = source[source.index('struct probe_work_stat {'):source.index('static void probe_note_work(')]
    backends = source[source.index('struct probe_identity {'):source.index('struct probe_helper {')]
    bodies = '\n\n'.join(function(source, name) for name in functions)
    harness = (ROOT / 'tests/transport_probe_harness.h').read_text()
    unit, binary = directory / 'probe-loop.c', directory / 'probe-loop'
    unit.write_text(harness.replace('/* PROBE_STATS */', stats)
                   .replace('/* PROBE_BACKENDS */', backends).replace('/* PROBE_FUNCTIONS */', bodies))
    built = subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                            str(unit), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    return subprocess.run([str(binary)], capture_output=True, text=True)


def check_probe_loop(directory):
    source = (ROOT / 'tests/transport_probe.h').read_text()
    assert 'probe_service_backends(&p);' in function(source, 'probe_run')
    assert 'probe_try_attach(' not in function(source, 'probe_record')
    assert 'b->closing' in function(source, 'probe_try_attach').split('return;', 1)[0]
    result = run_probe_loop(source, directory)
    assert result.returncode == 0, result.stdout + result.stderr
    stats = [line for line in result.stderr.splitlines() if line.startswith('PROBE-STAT:')]
    assert len(stats) == 22, stats
    assert stats[:4] == [f'PROBE-STAT: ringbuf_site_drops site={site} value={value}'
                         for site, value in zip(('LIFECYCLE_FORK', 'LIFECYCLE_EXIT', 'QUERY_TEXT',
                                                 'PG13_QUERY_TEXT'), (11, 13, 15, 17))], stats
    assert all(line.endswith('value=null') for line in stats[11:15]), stats
    assert stats[7:10] == [
        'PROBE-STAT: consume max_us=25 total_calls=2',
        'PROBE-STAT: lifecycle_rb in_callback_max_producer_minus_consumer_bytes=44000',
        'PROBE-STAT: teardown max_us=2 total=2'], stats
    assert stats[10] == 'PROBE-STAT: registry max_used=16 size=1024', stats
    assert ('PROBE-FAIL: backend slots exhausted used=1024 size=1024 closing=1024 '
            'pending=0 active=0 boot_fds=0 wait_fds=0') in result.stderr, result.stderr
    print(result.stdout, end='')
    # The real Linux driver removes its temporary log directory after each
    # run. Successful summaries must forward diagnostics before it does.
    from test_transport_producers_live import summary
    log = directory / 'probe.stderr'
    log.write_text('\n'.join(stats[:11]) + '\n')
    row = {'ok': True}
    class FinishedProbe:
        returncode = 0
        def communicate(self, timeout):
            assert timeout == 30
            return json.dumps(row) + '\n', None
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        assert summary(FinishedProbe(), log) == row
    assert output.getvalue().splitlines()[:11] == stats[:11], output.getvalue()
    print('probe diagnostics forwarding: PASS (all PROBE-STAT lines survive successful live summary)')
    # Restore per-backend link ownership/RCU teardown in real function bodies.
    # Run the time-driven churn case first: a built mutant must exhaust slots
    # at the named assertion, rather than merely fail an operation count.
    mutated = replace_once(source, '    bool pending, raced, closing;',
                           '    bool pending, raced, closing;\n    struct bpf_link *activity, *query;')
    attach = function(mutated, 'probe_attach_uprobes')
    per_pid = attach[attach.index('    if (p->activity_offset)'):attach.rindex('    return 0;')]
    per_pid = per_pid.replace('p->activity', 'b->activity').replace('p->query =', 'b->query =')
    per_pid = per_pid.replace('p->query)', 'b->query)').replace('p->query=NULL', 'b->query=NULL')
    per_pid = per_pid.replace('b->activity_offset', 'p->activity_offset')
    per_pid = per_pid.replace('-1, p->exe', '(pid_t)b->pid, p->exe').replace('return -1;', 'return;')
    instrument = function(mutated, 'probe_instrument')
    mutated = replace_once(mutated, instrument, instrument[:-1] + per_pid + '}')
    mutated = replace_once(mutated, attach, 'static int probe_attach_uprobes(struct probe *p) { (void)p; return 0; }')
    mutated = replace_once(mutated, '    if (b->boot >= 0) close(b->boot);',
                           '    bpf_link__destroy(b->activity); bpf_link__destroy(b->query);\n'
                           '    if (b->boot >= 0) close(b->boot);')
    result = run_probe_loop(mutated, directory)
    assertion = 'FAIL churn registry accepts backends without exhaustion'
    assert result.returncode != 0 and assertion in result.stderr, result.stdout + result.stderr
    assert 'backend slots exhausted used=1024 size=1024' in result.stderr, result.stderr
    print(f'mutant per-backend-uprobes-restored: KILLED ({assertion})')
    # Built mutation of the actual loop: both drains move after all backend
    # attaches, precisely the original reader-lag failure. The simulated
    # producers overrun their finite queue before that late drain runs.
    original = function(source, 'probe_service_backends')
    late = original.replace('            if (probe_drain(p) < 0 || p->failed) return;\n', '')
    late = late.replace('        if (probe_drain(p) < 0) return;\n', '')
    late = late[:-1] + '    probe_drain(p);\n}'
    result = run_probe_loop(replace_once(source, original, late), directory)
    assertion = 'FAIL lifecycle ring drained between attach batches'
    assert result.returncode != 0 and assertion in result.stderr, result.stdout + result.stderr
    print(f'mutant drain-after-all-attaches: KILLED ({assertion})')
    # Reintroduce the slow EXIT work inside the actual lifecycle callback.
    # It builds and handles an EXIT before the named runtime assertion fires.
    mutated = replace_once(source, '                p->backends[i].closing = true;',
                           '                probe_close_backend(&p->backends[i]);')
    result = run_probe_loop(mutated, directory)
    assertion = 'FAIL backend teardown runs outside lifecycle callback'
    assert result.returncode != 0 and assertion in result.stderr, result.stdout + result.stderr
    print(f'mutant teardown-in-callback: KILLED ({assertion})')
    # Freeing the PID at mark time permits reuse before the fd/link teardown.
    mutated = replace_once(source, '                p->backends[i].closing = true;',
                           '                p->backends[i].closing = true; p->backends[i].pid = 0;')
    result = run_probe_loop(mutated, directory)
    assertion = 'FAIL closing slot reserved until deferred teardown'
    assert result.returncode != 0 and assertion in result.stderr, result.stdout + result.stderr
    print(f'mutant reuse-before-teardown: KILLED ({assertion})')


def main():
    # Pure live-driver checks run on the Mac too; no BPF tools are invoked.
    from test_transport_producers_live import (PG13_LAYOUT, Refused, assert_clean_pg, layout,
                                               preemption_mode)
    # The live driver takes PostgreSQL offsets from the daemon's validated
    # table rather than from server headers. Pin its copy to that table.
    discovery = (ROOT / 'src/discovery.c').read_text()
    found = re.findall(r'case 13:\s*return (\d+);', function(discovery, 'pgwt_detect_pgproc_wait_offset'))
    assert found == [str(PG13_LAYOUT['pgproc_wait_event_info'])], found
    query = function(discovery, 'pgwt_detect_pg13_query_offsets')
    query = query[query.index('case 13:'):]
    for field in ('querydesc_plannedstmt', 'plannedstmt_queryid', 'querydesc_sourcetext'):
        found = re.findall(r'out->%s\s*=\s*(\d+);' % field, query)
        assert found == [str(PG13_LAYOUT[field])], (field, found)
    assert layout(13, 'x86_64') == (684, 8, 8, 16) and layout(17, 'x86_64') == (0, 0, 0, 0)
    assert layout(18, 'aarch64') == (0, 0, 0, 0)
    for major, machine in ((13, 'aarch64'), (16, 'x86_64'), (14, 'x86_64')):
        try:
            layout(major, machine)
        except Refused:
            pass
        else:
            raise AssertionError(f'layout accepted PG{major} on {machine}')
    print('live driver layout: PASS (PG13 offsets equal src/discovery.c; PG17+ direct pointer; unknown layouts refused)')
    for dynamic, config, expected in (
            ('none voluntary (full)', '', 'full'),
            ('none (voluntary) full', 'CONFIG_PREEMPT=y\nCONFIG_PREEMPT_DYNAMIC=y', 'voluntary'),
            ('(none) voluntary full', '', 'none'),
            ('', 'CONFIG_PREEMPT_DYNAMIC=y\nCONFIG_PREEMPT_NONE=y', 'unknown'),
            ('unreadable', '', 'unknown'),
            ('none voluntary [full]', 'CONFIG_PREEMPT_DYNAMIC=y', 'unknown'),
            ('', 'CONFIG_PREEMPT=y', 'full'),
            ('', 'CONFIG_PREEMPT_VOLUNTARY=y', 'voluntary'),
            ('', 'CONFIG_PREEMPT_NONE=y', 'none')):
        assert preemption_mode(dynamic, config) == expected, (dynamic, config)
    clean = dict(init_records=0, init_stored=1, attached=1, trace=1, failures=0, lost=0,
                 exited_before_attach=9, attach_exit_races=7, attach_failures=0,
                 cells=[dict(attempted=3, delivered=3, failed=0, discarded=0)])
    assert_clean_pg(clean)
    # Exit races are reported, not failures; an attach failure on a live
    # backend is.
    try:
        assert_clean_pg({**clean, 'attach_failures': 1})
    except AssertionError as error:
        assert error.args[0][0] == 'attach failed on a live backend', error
    else:
        raise AssertionError('live assertion accepted an attach failure on a live backend')
    # A counted discard is accepted; an uncounted hole or a failure is not.
    assert_clean_pg({**clean, 'cells': [dict(attempted=4, delivered=3, failed=0, discarded=1)]})
    for cell in (dict(attempted=4, delivered=3, failed=0, discarded=0),
                 dict(attempted=4, delivered=3, failed=1, discarded=0)):
        try:
            assert_clean_pg({**clean, 'cells': [cell]})
        except AssertionError:
            pass
        else:
            raise AssertionError(f'live assertion accepted {cell}')
    for key, value, assertion in (
            ('init_records', 1, 'zero INIT records on L under exact_perf_mode'),
            ('init_stored', 0, 'at least one cohort pid has INIT stored')):
        try:
            assert_clean_pg({**clean, key: value})
        except AssertionError as error:
            assert str(error) == assertion, error
        else:
            raise AssertionError(f'live assertion did not reject {key}={value}')
    # Execute the actual runner exclusion block in a tiny shell matrix. Its
    # continue must use exclude_test, just like the adjacent min_pg branch.
    runner = (ROOT / 'tests/run_all.sh').read_text()
    # Both transport live tests: the producers' probe and the daemon path.
    for live in ('test_transport_producers_live', 'test_transport_daemon_live'):
        assert f'"{live}|python3|{live}.py"' in runner, live
        start = runner.index(f'        if [[ "$name" == "{live}" &&')
        end = runner.index('\n        fi', start) + len('\n        fi')
        block = runner[start:end]
        script = '''exclude_test() { printf 'EXCLUDED %s PG%s\\n' "$1" "$PG_VERSION"; }
for PG_VERSION in 13 14 15 16 17 18; do
    name=''' + live + '''
''' + block + '''
    printf 'RUN PG%s\\n' "$PG_VERSION"
done
'''
        matrix = subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=True)
        assert matrix.stdout.splitlines() == [
            'RUN PG13', f'EXCLUDED {live} PG14', f'EXCLUDED {live} PG15', f'EXCLUDED {live} PG16',
            'RUN PG17', 'RUN PG18'], matrix.stdout
    print('live driver host checks: PASS (active preemption, missing evidence, INIT assertions, PG13-18 exclusion matrix)')
    source = (ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text()
    with tempfile.TemporaryDirectory(prefix='pgwt-producers-host-') as tmp:
        directory = Path(tmp)
        check_probe_loop(directory)
        result = run(source, directory)
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end='')
        # Take the marker's timestamp before its reservation. It compiles,
        # reaches a successful marker and fails the timestamp ordering check.
        early = replace_once(source,
            '    struct pgwt_trace_event evt = {};\n    u64 seq;\n'
            '    struct pgwt_wire_trace *wire = transport_reserve_trace(&seq);\n    if (!wire) {\n'
            '        transport_trace_failure(PGWT_WIRE_DEST_E, PGWT_RING_FAIL_MARKER);',
            '    struct pgwt_trace_event evt = {};\n    u64 seq;\n    u64 early = bpf_ktime_get_ns();\n'
            '    struct pgwt_wire_trace *wire = transport_reserve_trace(&seq);\n    if (!wire) {\n'
            '        transport_trace_failure(PGWT_WIRE_DEST_E, PGWT_RING_FAIL_MARKER);')
        early = replace_once(early, '    if (marker_edge(pid, marker, bpf_ktime_get_ns(), &evt)) {',
                             '    if (marker_edge(pid, marker, early, &evt)) {')
        mutants = [('timestamp-before-reserve', early, None, 'timestamp after reserve')]
        # FORK keeps adaptive wakeup so attachment can start promptly.
        fork = function(source, 'on_fork')
        mutants.append(('DATA-fork-wakeup-removed', replace_once(source, fork,
            replace_once(fork, 'bpf_ringbuf_submit(wire, 0);',
                         'bpf_ringbuf_submit(wire, BPF_RB_NO_WAKEUP);')),
            None, 'DATA fork L adaptive wakeup'))
        # Each other DATA submit site is exercised through its actual producer.
        # The two lifecycle EXIT branches share text but have separate cases.
        for name, producer, pointer, occurrence, assertion in (
                ('DATA-marker-wakeup', 'transport_marker', 'wire', 0, 'DATA marker E no-wakeup'),
                ('DATA-command-wakeup', 'transport_activity', 'wire', 0, 'DATA command E no-wakeup'),
                ('DATA-exit-E-wakeup', 'transport_exit', 'out', 0, 'DATA exit E no-wakeup'),
                ('DATA-exit-L-wakeup', 'transport_exit', 'life', 0, 'DATA exit L no-wakeup'),
                ('DATA-unattached-exit-L-wakeup', 'transport_exit', 'life', 1, 'DATA unattached exit L no-wakeup'),
                ('DATA-text-wakeup', 'transport_text', 'wire', 0, 'DATA text L no-wakeup')):
            body = function(source, producer)
            submit = f'bpf_ringbuf_submit({pointer}, BPF_RB_NO_WAKEUP);'
            positions = [match.start() for match in re.finditer(re.escape(submit), body)]
            assert len(positions) == (2 if pointer == 'life' else 1), name
            at = positions[occurrence]
            changed = body[:at] + f'bpf_ringbuf_submit({pointer}, 0);' + body[at + len(submit):]
            mutants.append((name, replace_once(source, body, changed), None, assertion))
        mutants.append(('BOUNDARY-wakeup-removed', replace_once(source,
            'bpf_ringbuf_submit(cut, BPF_RB_FORCE_WAKEUP);',
            'bpf_ringbuf_submit(cut, BPF_RB_NO_WAKEUP);'), None, 'BOUNDARY E forced wakeup'))
        # Adaptive wakeup is insufficient on either ring when DATA precedes
        # the cut. Check each destination at its own named assertion.
        for dest in ('E', 'L'):
            mutants.append((f'BOUNDARY-{dest}-adaptive-wakeup', replace_once(source,
                'bpf_ringbuf_submit(cut, BPF_RB_FORCE_WAKEUP);',
                f'bpf_ringbuf_submit(cut, dest == PGWT_WIRE_DEST_{dest} ? 0 : BPF_RB_FORCE_WAKEUP);'),
                None, f'BOUNDARY {dest} forced wakeup'))
        # Flag-off keeps E NO_WAKEUP and L adaptive, independently of the
        # exact-perf policy. Both mutants reach the raw submit shim.
        mutants.append(('mode-zero-E-wakeup', replace_once(source,
            '                                 BPF_RB_NO_WAKEUP);',
            '                                 0);'), None, 'mode zero submit flags unchanged'))
        mutants.append(('mode-zero-fork-wakeup-removed', replace_once(source, fork,
            replace_once(fork, 'bpf_ringbuf_submit(ev, 0);',
                         'bpf_ringbuf_submit(ev, BPF_RB_NO_WAKEUP);')),
            None, 'mode zero submit flags unchanged'))
        # Take the attempt number only when the reservation succeeded.
        mutants.append(('seq-only-on-success', replace_once(source,
            '    *seq = transport_attempt(PGWT_WIRE_DEST_E);\n'
            '    return bpf_ringbuf_reserve(&event_ringbuf, sizeof(struct pgwt_wire_trace), 0);',
            '    struct pgwt_wire_trace *ev = bpf_ringbuf_reserve(&event_ringbuf, sizeof(struct pgwt_wire_trace), 0);\n'
            '    *seq = ev ? transport_attempt(PGWT_WIRE_DEST_E) : 0;\n    return ev;'),
            None, 'marker fail attribution coherent'))
        header = (ROOT / 'src/bpf/transport_producer.h').read_text()
        # Inline the mutated production pure helper after resolving relative includes.
        header = header.replace('"../pgwt_transport_wire.h"', '"pgwt_transport_wire.h"').replace('"../pg_wait_tracer.h"', '"pg_wait_tracer.h"')
        mutants.append(('unpinned-helper', source, replace_once(header, '&& *expected == cpu', '&& cpu < MAX_CPUS'), 'helper wrong CPU refused'))
        # FORK must carry the child's saved lifetime, never the current
        # emitting task's (postmaster) PID entry. The retention check also
        # inspects the first envelope after the child PID has been reused.
        mutants.append(('current-pid-cookie', replace_once(source,
            '            seq, child_seed.cookie);', '            seq, transport_cookie(parent));'),
            None, 'queued cookie survives pid reuse'))
        mutants.append(('stale-cookie-on-reused-pid', replace_once(source,
            'transport_attempt(PGWT_WIRE_DEST_CPU), st->cookie);',
            'transport_attempt(PGWT_WIRE_DEST_CPU), 1);'), None, 'DATA cookie across PID reuse'))
        # Publication reads the entry again after the reservation, instead of
        # following the admission condition the caller read once.
        mutants.append(('command-double-read', replace_once(source,
            '    if (exact_admission_open()) {\n        wire->payload = (struct pgwt_trace_event){',
            '    if (exact_admission_open() && st->wp_live) {\n        wire->payload = (struct pgwt_trace_event){'),
            None, 'command saved admission releases reservation'))
        mutants.append(('INIT-publication-restored', restore_init_publication(source), None,
                        'zero INIT records on L under exact_perf_mode'))
        mutants.append(('admission-discard-counts-drop', replace_once(source,
            '        transport_discard(PGWT_WIRE_DEST_E);\n        bpf_ringbuf_discard(wire, 0);\n    }\n'
            '    if (pg13 && !open)',
            '        transport_trace_failure(PGWT_WIRE_DEST_E, PGWT_RING_FAIL_COMMAND_BOUNDARY);\n'
            '        bpf_ringbuf_discard(wire, 0);\n    }\n'
            '    if (pg13 && !open)'), None, 'command admission discard is not a ring drop'))
        # A by-design discard counted as a failure: the daemon would VOID a
        # run in which nothing was lost.
        mutants.append(('discard-counted-as-failure', replace_once(source,
            '        transport_discard(PGWT_WIRE_DEST_E);\n        bpf_ringbuf_discard(wire, 0);\n    }\n}\n',
            '        transport_failure(PGWT_WIRE_DEST_E);\n        bpf_ringbuf_discard(wire, 0);\n    }\n}\n'),
            None, 'marker admission discard is not a ring drop'))
        # An empty text counted as a failure, and an unreadable one as a
        # discard: both directions of the same confusion.
        mutants.append(('empty-text-counted-as-failure', replace_once(source,
            '        if (text_len < 0)\n            transport_failure(PGWT_WIRE_DEST_L);',
            '        if (text_len <= 1)\n            transport_failure(PGWT_WIRE_DEST_L);'),
            None, 'empty text is a discard, not a failure'))
        mutants.append(('unreadable-text-counted-as-discard', replace_once(source,
            '        if (text_len < 0)\n            transport_failure(PGWT_WIRE_DEST_L);',
            '        if (text_len < -1)\n            transport_failure(PGWT_WIRE_DEST_L);'),
            None, 'text discarded read failure'))
        # Under the mode nothing else wakes the daemon for a stored INIT.
        mutants.append(('INIT-wake-removed', replace_once(source,
            '            bpf_perf_event_output(ctx, &init_wake, BPF_F_CURRENT_CPU, &pid, sizeof(pid));\n', ''),
            None, 'stored INIT wakes the sweep once, with no attempt'))
        # The E/L issuer is the configured helper thread, not whoever asks.
        mutants.append(('any-helper-issues-el', replace_once(source,
            'if (tid == el_tid && progress && request && progress->el_request != request) {',
            'if (progress && request && progress->el_request != request) {'),
            None, 'only the configured helper thread issues E/L'))
        # EXIT with E refused: the L reservation taken afterwards is unused
        # and must be released, not left held.
        mutants.append(('exit-partial-leak', replace_once(source,
            '            if (unused) bpf_ringbuf_discard(unused, 0);\n', ''),
            None, 'partial exit discarded and counted'))
        for name, mutated, mutated_header, assertion in mutants:
            result = run(mutated, directory, mutated_header)
            assert result.returncode != 0 and 'FAIL ' + assertion in result.stderr, (name, result.stdout, result.stderr)
            print(f'mutant {name}: KILLED ({assertion})')
        print(f'producer mutants: {len(mutants)}/{len(mutants)} killed at intended assertions')


if __name__ == '__main__':
    main()
