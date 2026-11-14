#!/usr/bin/env python3
"""Compile real producer bodies against host shims; kill named built mutants.

Called by the registered C unit test (and make check). No BPF compilation/load.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ('count_ring_fail', 'count_map_fail', 'test_fail_once',
             'transport_cookie', 'transport_attempt', 'transport_header',
             'transport_failure', 'transport_trace_failure', 'transport_reserve_trace',
             'transport_reserve_lifecycle', 'transport_spin_failure',
             'transport_boundary', 'on_transport_helper', 'emit_event',
             'on_watchpoint', 'on_bootstrap', 'on_fork', 'transport_exit', 'on_exit',
             'emit_marker', 'on_report_activity', 'on_report_query_id', 'on_executor_start')


def function(source, name):
    matches = list(re.finditer(r'^(?:static __always_inline )?(?:void|bool|u64|int|struct pgwt_wire_(?:trace|lifecycle)) \**' + name + r'\(', source, re.M))
    assert len(matches) == 1, (name, len(matches))
    start = matches[0].start()
    brace = source.index('{', matches[0].end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def restore_init_publication(source):
    """The same built mutant is exercised by the host and real-PG drivers."""
    old = '            if (st && st->init_stored) return 0;\n            u64 stamp = bpf_ktime_get_ns();'
    new = '            if (st && st->init_stored) return 0;\n            struct pgwt_wire_lifecycle *wire = transport_reserve_lifecycle(pid,\n                pgwt_producer_cookie(st), PGWT_RING_FAIL_LIFECYCLE_INIT_HINT);\n            u64 stamp = bpf_ktime_get_ns();'
    assert source.count(old) == 1
    source = source.replace(old, new)
    old = '            } else count_map_fail(PGWT_BPF_FAIL_INIT_NO_STATE);\n            return 0;'
    new = '''            } else count_map_fail(PGWT_BPF_FAIL_INIT_NO_STATE);
            if (wire) {
                wire->payload = (struct pgwt_lifecycle_event){ .type = PGWT_LIFECYCLE_INIT,
                    .pid = pid, .addr = wait_addr, .timestamp = stamp };
                bpf_ringbuf_submit(wire, 0);
            }
            return 0;'''
    assert source.count(old) == 1
    return source.replace(old, new)


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


def main():
    # Pure live-driver checks run on the Mac too; no BPF tools are invoked.
    from test_transport_producers_live import preemption_mode, assert_clean_pg
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
                 cells=[dict(attempted=3, delivered=3, failed=0)])
    assert_clean_pg(clean)
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
    start = runner.index('        if [[ "$name" == "test_transport_producers_live" &&')
    end = runner.index('\n        fi', start) + len('\n        fi')
    block = runner[start:end]
    script = '''exclude_test() { printf 'EXCLUDED %s PG%s\\n' "$1" "$PG_VERSION"; }
for PG_VERSION in 13 14 15 16 17 18; do
    name=test_transport_producers_live
''' + block + '''
    printf 'RUN PG%s\\n' "$PG_VERSION"
done
'''
    matrix = subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=True)
    assert matrix.stdout.splitlines() == [
        'RUN PG13', 'EXCLUDED test_transport_producers_live PG14',
        'EXCLUDED test_transport_producers_live PG15', 'EXCLUDED test_transport_producers_live PG16',
        'RUN PG17', 'RUN PG18'], matrix.stdout
    print('live driver host checks: PASS (active preemption, missing evidence, INIT assertions, PG13-18 exclusion matrix)')
    source = (ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text()
    with tempfile.TemporaryDirectory(prefix='pgwt-producers-host-') as tmp:
        directory = Path(tmp)
        result = run(source, directory)
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout, end='')
        # Move marker's actual timestamp before its reservation. It compiles,
        # reaches a successful marker and fails the timestamp ordering check.
        old = '    if (exact_perf_mode) wire = transport_reserve_trace(pid, PGWT_RING_FAIL_MARKER);\n    u64 now = bpf_ktime_get_ns();'
        new = '    u64 now = bpf_ktime_get_ns();\n    if (exact_perf_mode) wire = transport_reserve_trace(pid, PGWT_RING_FAIL_MARKER);'
        assert source.count(old) == 1
        mutants = [('timestamp-before-reserve', source.replace(old, new), None, 'timestamp after reserve')]
        old = '    u64 seq = transport_attempt(PGWT_WIRE_DEST_E);\n    struct pgwt_wire_trace *ev = bpf_ringbuf_reserve(&event_ringbuf, sizeof(*ev), 0);'
        new = '    struct pgwt_wire_trace *ev = bpf_ringbuf_reserve(&event_ringbuf, sizeof(*ev), 0);\n    u64 seq = ev ? transport_attempt(PGWT_WIRE_DEST_E) : 0;'
        assert source.count(old) == 1
        mutants.append(('seq-only-on-success', source.replace(old, new), None, 'marker fail attribution coherent'))
        header = (ROOT / 'src/bpf/transport_producer.h').read_text()
        # Inline the mutated production pure helper after resolving relative includes.
        header = header.replace('"../pgwt_transport_wire.h"', '"pgwt_transport_wire.h"').replace('"../pg_wait_tracer.h"', '"pg_wait_tracer.h"')
        mutants.append(('unpinned-helper', source, header.replace('&& *expected == cpu', '&& cpu < MAX_CPUS'), 'helper wrong CPU refused'))
        # FORK must carry the child's saved lifetime, never the current
        # emitting task's (postmaster) PID entry. The retention check also
        # inspects the first envelope after the child PID has been reused.
        cookie_source = source.replace('child_seed.cookie, PGWT_RING_FAIL_LIFECYCLE_FORK);', 'transport_cookie(parent), PGWT_RING_FAIL_LIFECYCLE_FORK);')
        assert cookie_source != source
        mutants.append(('current-pid-cookie', cookie_source, None, 'queued cookie survives pid reuse'))
        stale_data = source.replace('transport_attempt(PGWT_WIRE_DEST_CPU), st->cookie);',
                                    'transport_attempt(PGWT_WIRE_DEST_CPU), 1);')
        assert stale_data != source
        mutants.append(('stale-cookie-on-reused-pid', stale_data, None, 'DATA cookie across PID reuse'))
        old = '    if (exact_perf_mode ? emit_boundary : (!lightweight_mode && st->wp_live)) {'
        assert source.count(old) == 1
        mutants.append(('command-double-read', source.replace(old, '    if (!lightweight_mode && st->wp_live) {'),
                        None, 'command saved admission releases reservation'))
        mutants.append(('INIT-publication-restored', restore_init_publication(source), None,
                        'zero INIT records on L under exact_perf_mode'))
        old = '                    transport_failure(PGWT_WIRE_DEST_E); }'
        assert source.count(old) == 1
        mutants.append(('admission-discard-counts-drop', source.replace(old,
                        '                    transport_trace_failure(PGWT_WIRE_DEST_E, PGWT_RING_FAIL_COMMAND_BOUNDARY); }'),
                        None, 'command admission discard is not a ring drop'))
        for name, mutated, mutated_header, assertion in mutants:
            result = run(mutated, directory, mutated_header)
            assert result.returncode != 0 and 'FAIL ' + assertion in result.stderr, (name, result.stdout, result.stderr)
            print(f'mutant {name}: KILLED ({assertion})')


if __name__ == '__main__':
    main()
