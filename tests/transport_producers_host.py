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
        for name, mutated, mutated_header, assertion in mutants:
            result = run(mutated, directory, mutated_header)
            assert result.returncode != 0 and 'FAIL ' + assertion in result.stderr, (name, result.stdout, result.stderr)
            print(f'mutant {name}: KILLED ({assertion})')


if __name__ == '__main__':
    main()
