#!/usr/bin/env python3
"""Portable startup diagnostics and skeleton-link guards; no kernel/BPF work."""
import errno
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from unittest.mock import patch

import transport_cutdiscard
from transport_cutdiscard import Refused, guard_objects, stamp_object

ROOT = Path(__file__).resolve().parents[1]
CC = shlex.split(os.environ.get('CC', 'cc'))
FLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror'] + shlex.split(os.environ.get('CFLAGS', ''))


def build(output, sources, extra=()):
    result = subprocess.run([*CC, *FLAGS, *extra, *map(str, sources), '-o', str(output)],
                            text=True, capture_output=True, cwd=ROOT)
    assert result.returncode == 0, result.stdout + result.stderr


def run(binary, case):
    return subprocess.run([str(binary), case], text=True, capture_output=True, timeout=10)


def counter_messages(work):
    binary = work / 'startup'
    build(binary, [ROOT / name for name in (
        'tests/transport_startup_host.c', 'src/percpu_transport.c', 'src/merge_capture.c',
        'src/core/merge_adapter.c', 'src/core/merge.c')], ['-DPGWT_TRANSPORT_HOST_TEST'])
    for case, component, error in [('map', 'wait_read_fail', str(errno.ENOTSUP)),
                                    ('unknown', 'lifecycle_init_drop', 'unknown'),
                                    ('snapshot', 'snapshot', 'unknown')]:
        result = run(binary, case)
        assert result.returncode == 1, (case, result)
        assert len(result.stderr.splitlines()) == 1, result.stderr
        assert f'startup refused: counter {component} is unreadable: errno={error}' in result.stderr, result.stderr
        print(f'startup counter {case}: exit=1; {result.stderr.strip()}')
    for case in ('healthy', 'disabled'):
        result = run(binary, case)
        assert result.returncode == 0 and result.stderr == '', (case, result)
    print('startup counter healthy/disabled controls: PASS')


def helper_messages(work):
    # Execute the exact helper body. Only Linux affinity, syscall and scheduler
    # queries are fake; mutexes, signals and error/ready propagation are real.
    transport = (ROOT / 'src/percpu_transport.c').read_text()
    body = transport[transport.index('static void *helper_loop('):transport.index('static int receive_ring(')]
    preamble = r'''
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
typedef unsigned test_cpu_set;
#define cpu_set_t test_cpu_set
#define CPU_SET_S(cpu, size, mask) ((void)(cpu), (void)(size), *(mask)=1)
#define CPU_ZERO_S(size, mask) ((void)(size), *(mask)=0)
#define CPU_COUNT_S(size, mask) ((void)(size), *(mask))
#define CPU_ISSET_S(cpu, size, mask) ((void)(cpu), (void)(size), *(mask)==1)
#define sched_setaffinity(pid, size, mask) ((void)(pid), (void)(size), (void)(mask), 0)
#define sched_getaffinity(pid, size, mask) ((void)(pid), (void)(size), *(mask)=1, 0)
#define sched_getcpu() 0
#define SYS_gettid 1
#define SYS_getppid 2
#define syscall(nr) ((void)(nr), 123)
static int test_policy, test_error;
static int test_getschedparam(pthread_t thread, int *policy, struct sched_param *param)
{ (void)thread; (void)param; *policy=test_policy; return test_error; }
#define pthread_getschedparam test_getschedparam
struct pinned_helper {
    size_t mask_size; unsigned cpu; uint32_t tid;
    int error; bool ready, stop, wake;
    pthread_mutex_t mutex; pthread_cond_t cond;
};
'''
    main = r'''
int main(int argc, char **argv)
{
    if (argc!=2) return 2;
    int arm=atoi(argv[1]);
    test_policy=arm==0?SCHED_OTHER:arm==1?SCHED_FIFO:SCHED_RR;
    if (arm==3) test_error=EPERM;
    struct pinned_helper h={.mask_size=sizeof(test_cpu_set),.stop=true,
        .mutex=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};
    helper_loop(&h);
    if (!h.ready || h.tid!=123 || h.error!=(arm==0?0:arm==3?EPERM:EINVAL)) return 2;
    pthread_cond_destroy(&h.cond); pthread_mutex_destroy(&h.mutex);
    return h.error?1:0;
}
'''
    source = work / 'helper.c'
    source.write_text(preamble + body + main)
    binary = work / 'helper'
    build(binary, [source], ['-pthread'])
    for arm in ('1', '2'):
        result = run(binary, arm)
        assert result.returncode == 1 and len(result.stderr.splitlines()) == 1, result
        assert 'real-time scheduling of the tracer is not supported yet' in result.stderr, result.stderr
        assert 'requires SCHED_OTHER' in result.stderr, result.stderr
        print(f'startup helper arm={arm}: exit=1; {result.stderr.strip()}')
    for arm, status in [('0', 0), ('3', 1)]:
        result = run(binary, arm)
        assert result.returncode == status and result.stderr == '', result
    print('startup helper SCHED_OTHER/query-error controls: PASS')


def skeleton_guards(work):
    # Two header layouts with swapped map slots reproduce the original ABI
    # mismatch without generating or loading a BPF object.
    header = work / 'pg_wait_tracer.skel.h'
    other = work / 'other'
    other.mkdir()
    wrong = other / header.name
    header.write_text('struct maps { int init_wake, fail_counters; };\n')
    wrong.write_text('struct maps { int fail_counters, init_wake; };\n')
    source = work / 'layout.c'
    source.write_text('#include "pg_wait_tracer.skel.h"\nint slot(struct maps *m) { return m->fail_counters; }\n')
    good_obj, bad_obj = work / 'good.o', work / 'bad.o'
    for obj, selected in [(good_obj, header), (bad_obj, wrong)]:
        # Compile from a directory without a sibling header so -I controls it.
        unit = other / 'unit.c' if obj == good_obj else work / 'unit.c'
        unit.write_text(source.read_text().replace('"pg_wait_tracer.skel.h"', '<pg_wait_tracer.skel.h>'))
        build(obj, [unit], ['-I', str(selected.parent), '-MMD', '-MF', str(obj.with_suffix('.d')), '-c'])
        stamp_object(obj, obj.with_suffix('.d'), selected)
    guard_objects([good_obj], header)

    def refuses(label, callback):
        try:
            callback()
        except Refused as error:
            assert 'skeleton mismatch:' in str(error), error
            print(f'skeleton guard {label}: REFUSED')
        else:
            raise AssertionError(f'skeleton guard accepted {label}')

    refuses('different layout', lambda: guard_objects([good_obj, bad_obj], header))
    refuses('wrong compiler dependency', lambda: stamp_object(bad_obj, bad_obj.with_suffix('.d'), header))
    product = work / 'product.o'
    product.write_bytes(good_obj.read_bytes())
    refuses('unproven product object', lambda: guard_objects([product], header))
    good_obj.write_bytes(bad_obj.read_bytes())
    refuses('object substituted after compile', lambda: guard_objects([good_obj], header))
    print('skeleton build/link guards: PASS')

    # Exercise build() itself with ordinary C and a fixture header in place of
    # BPF tooling. A mismatched object must refuse BEFORE the linker is called.
    main_source = work / 'main.c'
    main_source.write_text('int main(void) { return 0; }\n')
    config = dict(CC=shlex.join(CC), CFLAGS=shlex.join(FLAGS), LDFLAGS='',
                  USER_SRCS=shlex.join([str(work / 'unit.c'), str(main_source)]),
                  USER_OBJS='build/unit.o build/main.o',
                  CLANG='fixture-clang', BPF_CFLAGS='', BPFTOOL='fixture-bpftool')
    real_checked = transport_cutdiscard.checked
    link_calls = []

    def fixture_tools(command, **kwargs):
        if command[0] == 'make':
            return '\n'.join(f'PGWT_CONFIG_{key}={value}' for key, value in config.items())
        if command[0] == 'fixture-clang':
            return ''
        if command[0] == 'fixture-bpftool':
            return header.read_text()
        if '-c' not in command:
            link_calls.append(command)
        return real_checked(command, **kwargs)

    with patch.object(transport_cutdiscard, 'checked', fixture_tools):
        binary = transport_cutdiscard.build(work, 1)
        assert binary.is_file() and len(link_calls) == 1
        assert (work / 'cutdiscard/objects/build/unit.o').is_file()
        assert (work / 'cutdiscard/objects/build/main.o').is_file()
        link_calls.clear()

        def stale_stamp(obj, dependencies, skeleton):
            stamp_object(obj, dependencies, skeleton)
            stamp_path = Path(str(obj) + '.skeleton.json')
            stamp = json.loads(stamp_path.read_text())
            stamp['skeleton'] = transport_cutdiscard.fingerprint(wrong)
            stamp_path.write_text(json.dumps(stamp))

        with patch.object(transport_cutdiscard, 'stamp_object', stale_stamp):
            refuses('build refuses before link', lambda: transport_cutdiscard.build(work, 1, True))
        assert link_calls == [], link_calls
    print('cutdiscard build: all userspace objects rebuilt; mismatched provenance refused before link PASS')


def main():
    with tempfile.TemporaryDirectory(prefix='pgwt-transport-startup-') as temporary:
        work = Path(temporary)
        counter_messages(work)
        helper_messages(work)
        skeleton_guards(work)


if __name__ == '__main__':
    main()
