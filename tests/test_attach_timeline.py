#!/usr/bin/env python3
"""Portable attachment timing, JSON goldens, production hooks and built mutants.

Only fake clocks/resource usage and extracted userspace functions run here.
No BPF loader, kernel watchpoint, live PostgreSQL or generated repo file.
"""
import ast
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_backend_init_order import function
from test_backend_attach_race import HARNESS as ATTACH_HARNESS
from test_init_loss_diagnostics import HARNESS as LOSS_HARNESS

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'src/attach_timeline.h'

SYNTHETIC = r'''
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 12345
#endif
static int usage_calls, fail_usage, fail_alloc;
static int fake_getrusage(int who, struct rusage *u)
{
    if (who != RUSAGE_THREAD) exit(2);
    ++usage_calls;
    errno = EACCES;
    if (fail_usage) return -1;
    *u = (struct rusage){.ru_nvcsw=usage_calls * 10, .ru_nivcsw=usage_calls * 10 + 1};
    return 0;
}
static void *fake_realloc(void *p, size_t n) { return fail_alloc ? NULL : realloc(p, n); }
#define getrusage fake_getrusage
#define realloc fake_realloc
#include "attach_timeline.h"
#undef getrusage
#undef realloc
static uint64_t now;
uint64_t pgwt_debug_monotonic_ns(void) { errno=EIO; return now++; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s\n",#x); exit(1); } } while (0)
static void receive(struct pgwt_attach_timeline *t, unsigned pid, uint64_t cookie,
    unsigned type, uint64_t ns, unsigned flags, uint64_t init)
{
    struct pgwt_lifecycle_event e = {.pid=pid,.type=type,.timestamp=ns,.exit_flags=flags,.addr=init};
    pgwt_timeline_dequeue(t);
    pgwt_timeline_receive(t,&e,cookie);
}
int main(void)
{
    struct pgwt_attach_timeline *t=calloc(1,sizeof(*t)); CHECK(t);
    unsigned delay[]={3,1,10,2,20,9,8,7,6,5};
    for (unsigned i=0;i<10;++i) {
        uint64_t fork=1000+100000*i, d=delay[i];
        unsigned pid=42+i;
        t->epoll_return_ns=fork+1; t->batch_records=7;
        now=fork+d; receive(t,pid,7+i,PGWT_LIFECYCLE_FORK,fork,0,0);
        now=fork+11*d; errno=ENOSPC;
        pgwt_timeline_handler(t,pid,7+i,true); CHECK(errno==ENOSPC);
        now=fork+11*d+1; pgwt_timeline_phase(t,pid,7+i,PGWT_TL_BOOT_START,true);
        now=fork+111*d; pgwt_timeline_phase(t,pid,7+i,PGWT_TL_BOOT_END,i!=9);
        now=fork+112*d; pgwt_timeline_handler(t,pid,7+i,false); CHECK(errno==ENOSPC);
        if (i<4) {
            now=fork+500*d; pgwt_timeline_phase(t,pid,7+i,PGWT_TL_WAIT_OPEN,true);
            now=fork+1000*d; pgwt_timeline_phase(t,pid,7+i,PGWT_TL_WAIT_ENABLE,true);
        } else pgwt_timeline_loss(t,pid,7+i);
        unsigned flags=i==9 ? 0 : PGWT_EXIT_DISPOSITION;
        if (i<4 || i==6 || i==7 || i==8) flags |= PGWT_EXIT_BOOTSTRAP_FIRED;
        if (i<4 || i==6 || i==7) flags |= PGWT_EXIT_INIT_STORED;
        now=fork+1001*d;
        receive(t,pid,7+i,PGWT_LIFECYCLE_EXIT,fork+1000*d+1,flags,
                flags & PGWT_EXIT_INIT_STORED ? fork+2 : 0);
        errno=EBUSY;
        pgwt_timeline_finish(stdout,t,pid,7+i); CHECK(errno==EBUSY);
        pgwt_timeline_finish(stdout,t,pid,7+i); /* duplicate: one line */
    }
    CHECK(t->pending==0 && t->completed==10 && t->count==10 && usage_calls==20);
    pgwt_timeline_summary(stdout,t);
    free(t->samples); free(t);

    /* Missing timestamps/usage, cookie isolation, failed then successful
     * existing attempts, default-wire PID reuse, and explicit overflow. */
    t=calloc(1,sizeof(*t)); CHECK(t);
    now=200; receive(t,42,7,PGWT_LIFECYCLE_FORK,100,0,0);
    now=210; receive(t,42,8,PGWT_LIFECYCLE_FORK,101,0,0);
    pgwt_timeline_loss(t,42,7);
    CHECK(!pgwt_timeline_find(t,42,8,false)->lost);
    fail_usage=1; now=220; pgwt_timeline_handler(t,42,7,true);
    CHECK(!pgwt_timeline_find(t,42,7,false)->entry_usage_valid);
    now=230; pgwt_timeline_phase(t,42,7,PGWT_TL_WAIT_ENABLE,false);
    now=240; pgwt_timeline_phase(t,42,7,PGWT_TL_WAIT_ENABLE,true);
    CHECK(pgwt_timeline_find(t,42,7,false)->wait_enable_ns==240);
    now=250; pgwt_timeline_phase(t,42,7,PGWT_TL_WAIT_ENABLE,true);
    CHECK(pgwt_timeline_find(t,42,7,false)->wait_enable_ns==240);
    /* No EXIT: never manufacture an exit time or a completed sample. */
    pgwt_timeline_finish(stdout,t,42,7); CHECK(!t->completed);
    now=300; receive(t,50,0,PGWT_LIFECYCLE_FORK,110,0,0);
    now=400; receive(t,50,0,PGWT_LIFECYCLE_FORK,120,0,0);
    CHECK(t->replaced_pending==1 && pgwt_timeline_find(t,50,0,false)->fork_ns==120);
    free(t);
    t=calloc(1,sizeof(*t)); CHECK(t);
    struct pgwt_timeline_row *zero=pgwt_timeline_find(t,1,1,true); CHECK(zero);
    zero->fork_ns=100; zero->rb_receive_ns=100; zero->handler_entry_ns=90; zero->exit_ns=200;
    FILE *zero_sink=tmpfile(); CHECK(zero_sink);
    pgwt_timeline_finish(zero_sink,t,1,1);
    CHECK(t->samples[0].valid==1 && t->samples[0].duration[0]==0);
    pgwt_timeline_summary(zero_sink,t);
    rewind(zero_sink);
    char text[16384]; size_t length=fread(text,1,sizeof(text)-1,zero_sink); text[length]=0;
    CHECK(strstr(text,"\"fork_to_receive_ns\":{\"n\":1,\"p50\":0,\"p90\":0,\"p99\":0,\"max\":0}"));
    CHECK(strstr(text,"\"receive_to_handler_ns\":{\"n\":0,\"p50\":null"));
    fclose(zero_sink); free(t->samples); free(t);
    t=calloc(1,sizeof(*t)); CHECK(t);
    for (unsigned i=1;i<=PGWT_TIMELINE_SLOTS;++i) CHECK(pgwt_timeline_find(t,i,1,true));
    CHECK(!pgwt_timeline_find(t,999999,1,true) && t->table_overflows==1);
    struct pgwt_timeline_row *r=pgwt_timeline_find(t,1,1,false); CHECK(r);
    r->exit_ns=100; fail_alloc=1;
    FILE *sink=tmpfile(); CHECK(sink);
    pgwt_timeline_finish(sink,t,1,1);
    CHECK(t->summary_dropped==1 && t->completed==1 && !t->count);
    CHECK(pgwt_timeline_find(t,1+PGWT_TIMELINE_SLOTS,1,true));
    fclose(sink); free(t);
    puts("REACHED timeline edges: nulls, zero/reversed deltas, cookies, retries, pending, reuse, overflow, allocation failure");
    return 0;
}
'''


REUSE = r'''
#include "attach_timeline.h"
static uint64_t now = 1;
uint64_t pgwt_debug_monotonic_ns(void) { return ++now; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s\n",#x); exit(1); } } while (0)
static void rx(struct pgwt_attach_timeline *t, unsigned type, uint64_t ts)
{
    struct pgwt_lifecycle_event e = {.type=type,.pid=500,.timestamp=ts};
    pgwt_timeline_dequeue(t); pgwt_timeline_receive(t,&e,0);
}
int main(void)
{
    struct pgwt_attach_timeline *t=calloc(1,sizeof(*t)); CHECK(t);
    /* Reviewer's reproduction: pre-existing backend exits without a FORK. */
    rx(t,PGWT_LIFECYCLE_EXIT,1000);
    pgwt_timeline_finish(stdout,t,500,0);
    rx(t,PGWT_LIFECYCLE_FORK,2000);
    pgwt_timeline_handler(t,500,0,true);
    pgwt_timeline_phase(t,500,0,PGWT_TL_WAIT_ENABLE,true);
    rx(t,PGWT_LIFECYCLE_EXIT,3000);
    pgwt_timeline_finish(stdout,t,500,0);
    puts("REACHED completed PID reuse after forkless EXIT");
    CHECK(t->completed==2 && t->pending==0 && t->table_overflows==0);
    pgwt_timeline_summary(stdout,t);
    free(t->samples); free(t);
    return 0;
}
'''

PROBES = r'''
#include <stdio.h>
#include <stdlib.h>
static unsigned probes;
#include "attach_timeline.h"
uint64_t pgwt_debug_monotonic_ns(void) { return 1; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s\n",#x); exit(1); } } while (0)
int main(void)
{
    struct pgwt_attach_timeline *t=calloc(1,sizeof(*t)); CHECK(t);
    unsigned maximum=0; uint64_t total=0;
    /* Sequential cookies, same PID: completed history saturates the table. */
    for (unsigned i=1;i<=20000;++i) {
        probes=0;
        struct pgwt_timeline_row *r=pgwt_timeline_find(t,42,i,true); CHECK(r);
        total+=probes; if (probes>maximum) maximum=probes;
        r->slot=2; --t->pending;
    }
    printf("REACHED 20000 backends: insert probes total=%llu max=%u\n",
           (unsigned long long)total,maximum);
    CHECK(maximum<=64 && t->table_overflows==0 && t->pending==0);
    free(t); t=calloc(1,sizeof(*t)); CHECK(t);
    /* Force one collision chain. Reuse must not shadow an existing key. */
    for (unsigned i=1;i<=64;++i)
        CHECK(pgwt_timeline_find(t,i*PGWT_TIMELINE_SLOTS,0,true));
    struct pgwt_timeline_row *first=pgwt_timeline_find(t,PGWT_TIMELINE_SLOTS,0,false);
    struct pgwt_timeline_row *last=pgwt_timeline_find(t,64*PGWT_TIMELINE_SLOTS,0,false);
    CHECK(first && last); first->slot=2; --t->pending;
    probes=0; CHECK(pgwt_timeline_find(t,64*PGWT_TIMELINE_SLOTS,0,true)==last);
    CHECK(probes==64 && first->slot==2 && t->pending==63);
    CHECK(pgwt_timeline_find(t,65*PGWT_TIMELINE_SLOTS,0,true)==first);
    probes=0; CHECK(!pgwt_timeline_find(t,66*PGWT_TIMELINE_SLOTS,0,true));
    CHECK(probes==64 && t->table_overflows==1 && t->pending==64);
    probes=0; CHECK(!pgwt_timeline_find(t,66*PGWT_TIMELINE_SLOTS,0,false));
    CHECK(probes==64 && t->table_overflows==1);
    pgwt_timeline_summary(stdout,t);
    free(t);
    puts("REACHED collision, tombstone lookup/reuse and counted bounded overflow");
    return 0;
}
'''


def compile_run(directory, name, source, header=None, extra=(), intended=None):
    if header is not None:
        (directory / 'attach_timeline.h').write_text(header)
        # Quoted includes in daemon.h must select the shadow timeline too.
        (directory / 'daemon.h').write_text((ROOT / 'src/daemon.h').read_text())
    cfile, binary = directory / (name + '.c'), directory / name
    cfile.write_text(source)
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-I' + str(directory), '-I' + str(ROOT / 'src'),
               *shlex.split(os.environ.get('CFLAGS', '')), str(cfile), *map(str, extra), '-o', str(binary)]
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:exitcode=86',
               UBSAN_OPTIONS='halt_on_error=1:exitcode=87:print_stacktrace=1')
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    assert not any(marker in result.stderr for marker in (
        'AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:')), result.stderr
    if intended:
        assert result.returncode == 1 and intended in result.stderr, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return result


def parse(text):
    # Execute only the pure parser, without importing the live reproduction.
    module = ast.parse((ROOT / 'tests/init_loss_repro.py').read_text())
    node = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == 'attach_timeline_records')
    namespace = {'json': json}
    exec(compile(ast.Module(body=[node], type_ignores=[]), '<timeline parser>', 'exec'), namespace)
    return namespace['attach_timeline_records'](text)


def check_fields(result):
    parsed = parse(result.stdout)
    rows = parsed['timelines']
    assert len(rows) == 10, 'FAIL completed line count'
    expected = dict(clock='CLOCK_MONOTONIC', pid=42, cookie=7, identity='cookie', outcome='attached', lost=False,
                    fork_ns=1000, rb_receive_ns=1003, epoll_return_ns=1001, batch_records_before=7,
                    handler_entry_ns=1033, handler_exit_ns=1336, bootstrap_open_start_ns=1034,
                    bootstrap_open_ns=1333, bootstrap_fire_ns=1002, wait_open_ns=2500, wait_enable_ns=4000,
                    exit_ns=4001, handler_entry_nvcsw=10, handler_entry_nivcsw=11,
                    handler_exit_nvcsw=20, handler_exit_nivcsw=21,
                    bootstrap_open_ok=True, wait_open_ok=True, wait_enable_ok=True)
    for key, value in expected.items():
        assert rows[0][key] == value, f'FAIL field {key}: {rows[0][key]} != {value}'
    assert set(rows[0]) == set(expected), 'FAIL field set'
    assert [row['outcome'] for row in rows] == ['attached'] * 4 + ['never_fired'] * 2 + ['fired+stored'] * 2 + ['fired+not_stored', 'unknown']
    for i in (4, 5, 8, 9):
        assert rows[i]['bootstrap_fire_ns'] is None, 'FAIL missing bootstrap timestamp'
        assert rows[i]['wait_enable_ns'] is None and rows[i]['wait_enable_ok'] is None
    assert rows[6]['bootstrap_fire_ns'] == 601002
    assert rows[9]['bootstrap_open_ok'] is False
    assert parsed['stats'] == [dict(completed=10, pending=0, table_overflows=0, summary_dropped=0,
                                   replaced_pending=0, summary_allocation_ok=True, outer_loop_wakes=0,
                                   outer_loop_lifecycle_wakes=0, outer_loop_elapsed_ns=0, pgbs_resolve_calls=0,
                                   pgbs_resolve_misses=0, pgbs_resolve_total_ns=0,
                                   pgbs_resolve_extent_fallbacks=0)]
    return parsed


def check_summaries(result):
    rows = parse(result.stdout)['summaries']
    assert len(rows) == 12, 'FAIL summary groups'
    by_group = {(r['population'], r['outcome']): r for r in rows}
    all_rows = by_group['all', 'all']
    assert all_rows['backends'] == 10, 'FAIL summary all population'
    # Hand-calculated nearest ranks, with deliberately unsorted inputs.
    expected = {
        'fork_to_receive_ns': dict(n=10, p50=6, p90=10, p99=20, max=20),
        'receive_to_handler_ns': dict(n=10, p50=60, p90=100, p99=200, max=200),
        'handler_to_bootstrap_armed_ns': dict(n=9, p50=700, p90=2000, p99=2000, max=2000),
        'fork_to_wait_enabled_ns': dict(n=4, p50=2000, p90=10000, p99=10000, max=10000),
    }
    for key, value in expected.items():
        assert all_rows[key] == value, f'FAIL summary {key}: {all_rows[key]} != {value}'
    lost = by_group['lost', 'all']
    assert lost['backends'] == 6, 'FAIL summary lost population'
    assert lost['fork_to_receive_ns'] == dict(n=6, p50=7, p90=20, p99=20, max=20), 'FAIL summary lost ranks'
    assert lost['fork_to_wait_enabled_ns'] == dict(n=0, p50=None, p90=None, p99=None, max=None)
    assert by_group['lost', 'attached']['backends'] == 0
    assert by_group['all', 'fired+stored']['fork_to_receive_ns'] == dict(n=2, p50=7, p90=8, p99=8, max=8)


def production_hooks(directory, backend=None, daemon=None, intended=None):
    backend = (ROOT / 'src/backend.c').read_text() if backend is None else backend
    daemon = (ROOT / 'src/daemon.c').read_text() if daemon is None else daemon
    cmdline = (ROOT / 'src/cmdline.c').read_text()
    functions = '\n\n'.join([
        function(cmdline, 'pgwt_backend_target_live_fd'),
        function(cmdline, 'pgwt_backend_no_command_fd'), function(cmdline, 'pgwt_backend_target_live'),
        *(function(backend, n) for n in ('open_watchpoint_target', 'record_watchpoint_failure',
                                       'pgwt_attach_backend_watchpoint', 'enrollment_failure_counted',
                                       'enrollment_was_refused', 'remember_enrollment_refusal',
                                       'lifecycle_target', 'pgwt_handle_fork'))])
    # Compile the actual FORK switch arm, including both handler arguments.
    # Other lifecycle arms have unrelated query-text and map dependencies.
    dispatch = function(daemon, 'process_lifecycle_event')
    fork_arm = dispatch.split('    case PGWT_LIFECYCLE_FORK:', 1)[1].split(
        '    case PGWT_LIFECYCLE_INIT:', 1)[0]
    functions += ('\nstatic void dispatch_fork(struct pgwt_daemon *d, struct pgwt_lifecycle_event *ev)\n'
                  '{ const char *op = NULL; switch (ev->type) { case PGWT_LIFECYCLE_FORK:'
                  + fork_arm + '} (void)op; }\n')
    # Master enables block timing in the errno harness. Keep this diagnostic
    # off here so the clock counter measures attachment timeline work alone.
    source = ATTACH_HARNESS.replace('@FUNCTIONS@', functions).replace(
        'd.debug_dump_state = true;', 'd.debug_dump_state = false;')
    source = source.replace('uint64_t pgwt_debug_monotonic_ns(void) { return 100; }',
                            'static int timeline_clocks;\nuint64_t pgwt_debug_monotonic_ns(void) { return 100 + ++timeline_clocks; }')
    before, end = source.rsplit('    return 0;\n}', 1)
    assert not end.strip()
    source = before + r'''
    reset(LIVE);
    struct pgwt_lifecycle_event fork={.type=PGWT_LIFECYCLE_FORK,.pid=42};
    dispatch_fork(&d,&fork);
    CHECK(timeline_clocks==0,"off path adds no timeline clocks");
    reset(LIVE);
    d.attach_timeline=calloc(1,sizeof(*d.attach_timeline)); CHECK(d.attach_timeline,"timeline allocated");
    struct pgwt_timeline_row *r=pgwt_timeline_find(d.attach_timeline,42,0,true);
    CHECK(r,"timeline row");
    dispatch_fork(&d,&fork);
    puts("REACHED production FORK handler and bootstrap failure");
    CHECK(attempts==1 && d.counters.wp_attach_failures_total==1,"original bootstrap failure");
    CHECK(r->handler_entry_ns==101 && r->handler_exit_ns==104,"production handler timestamps");
    CHECK(r->bootstrap_open_start_ns==102 && r->bootstrap_open_ns==103 &&
          !r->bootstrap_open_ok,"production bootstrap timestamps");
    operation_failed=0; enable_failure=2;
    be=(struct pgwt_backend){.pid=42,.wp_addr=1234,.wp_fd=-1};
    CHECK(pgwt_attach_backend_watchpoint(&d,&be)==0,"original successful attach");
    puts("REACHED production wait open and enable");
    CHECK(r->wait_open_ns==105 && r->wait_enable_ns==106 && r->wait_enable_ok,
          "production wait open and enable timestamps");
    CHECK(timeline_clocks==6,"only enabled diagnostic clocks");
    free(d.attach_timeline);
    puts("REACHED production bootstrap/wait hooks; off adds zero clocks; I/O decisions preserved");
    return 0;
}
'''
    result = compile_run(directory, 'attach_hooks', source, intended=intended)
    if intended:
        assert 'REACHED production FORK handler' in result.stdout
        if 'wait open' in intended:
            assert 'REACHED production wait open and enable' in result.stdout
        return
    print(result.stdout, end='')

    transport = (ROOT / 'src/percpu_transport.c').read_text()
    functions = '\n'.join([
        function(backend, 'enrollment_failure_counted'),
        function(backend, 'forget_enrollment_refusal'),
        function(backend, 'record_init_loss'), function(backend, 'pgwt_handle_exit'),
        function(backend, 'pgwt_handle_exit_at'), function(daemon, 'pgwt_process_lifecycle_payload'),
        function(transport, 'linux_received')])
    source = LOSS_HARNESS.replace('@FUNCTIONS@', functions).split('int main(void)\n{')[0] + r'''
int main(void)
{
    /* Keep every existing shim reachable, without changing product bodies. */
    (void)trace;
    reset(); read_error=-ENOENT;
    d.attach_timeline=calloc(1,sizeof(*d.attach_timeline)); CHECK(d.attach_timeline,"timeline allocated");
    d.attach_timeline->receive_ns=110; d.attach_timeline->epoll_return_ns=105;
    d.attach_timeline->records_before=4;
    receipt(PGWT_LIFECYCLE_FORK,100,7);
    struct pgwt_timeline_row *r=pgwt_timeline_find(d.attach_timeline,42,7,false);
    CHECK(r && r->fork_ns==100 && r->rb_receive_ns==110 && r->epoll_return_ns==105 &&
          r->batch_records_before==4,"receipt stamped before capture disposition");
    /* Retire before receiving EXIT. The same cookie must retain the loss. */
    pgwt_handle_exit(&d,42); CHECK(r->lost && !d.attach_timeline->completed,"loss retained until EXIT");
    exit_disposition(8,PGWT_EXIT_DISPOSITION,0);
    CHECK(r->lost && r->slot==1,"replacement EXIT isolated");
    exit_disposition(7,PGWT_EXIT_DISPOSITION|PGWT_EXIT_BOOTSTRAP_FIRED|PGWT_EXIT_INIT_STORED,123);
    CHECK(r->slot==2 && d.attach_timeline->completed==2 && d.counters.lifecycle_init_lost_total==1,
          "late EXIT completes exactly once without changing accounting");
    exit_disposition(7,PGWT_EXIT_DISPOSITION,0);
    CHECK(d.attach_timeline->completed==2,"duplicate EXIT ignored");
    free(d.attach_timeline->samples); free(d.attach_timeline);
    reset(); d.exact_perf=false; d.backends.entries[0].init_diag_cookie=0;
    d.attach_timeline=calloc(1,sizeof(*d.attach_timeline)); CHECK(d.attach_timeline,"default timeline");
    d.attach_timeline->receive_ns=110;
    struct pgwt_lifecycle_event fork={.pid=42,.type=PGWT_LIFECYCLE_FORK,.timestamp=100};
    pgwt_process_lifecycle_payload(&d,&fork,sizeof(fork));
    CHECK(pgwt_timeline_find(d.attach_timeline,42,0,false)->rb_receive_ns==110,"default FORK receipt");
    read_error=-ENOENT; sweep_exit=1;
    struct pgwt_lifecycle_event exit_record={.pid=42,.type=PGWT_LIFECYCLE_EXIT,.timestamp=300,
        .exit_flags=PGWT_EXIT_DISPOSITION};
    pgwt_process_lifecycle_payload(&d,&exit_record,sizeof(exit_record));
    CHECK(d.attach_timeline->completed==1 && d.attach_timeline->samples[0].lost,
          "pre-record sweep loss included in default EXIT line");
    free(d.attach_timeline->samples); free(d.attach_timeline);
    puts("REACHED production receipt/late EXIT/default sweep hooks; counters preserved");
    return 0;
}
'''
    result = compile_run(directory, 'receipt_hooks', source, extra=(ROOT / 'src/backend_init.c',))
    print(result.stdout, end='')
    rows = parse(result.stderr)['timelines']
    assert len(rows) == 3 and rows[1]['lost'] and rows[1]['outcome'] == 'fired+stored'
    assert rows[2]['identity'] == 'pid_only' and rows[2]['lost']

    # Real epoll/consume seams are Linux-only. Pin their timing/reset placement
    # as well as compiling the production receipt and backend bodies above.
    callback = function(transport, 'receive_ring')
    assert callback.index('pgwt_timeline_dequeue(') < callback.index('pgwt_transport_ring_record(')
    assert 'attach_timeline && r->dest == PGWT_WIRE_DEST_L' in callback
    assert 'attach_timeline->batch_records = 0;' in function(transport, 'consume_all')
    assert 'attach_timeline->batch_records = 0;' in function(transport, 'linux_consume_io')
    assert 'attach_timeline->batch_records = 0;' in function(daemon, 'pgwt_consume_lifecycle_ring')
    assert 'pgwt_timeline_dequeue(' in function(daemon, 'handle_lifecycle_event')
    dispatch = function(daemon, 'process_lifecycle_event')
    assert dispatch.index('pgwt_timeline_handler(') < dispatch.index('pgwt_handle_fork(') < dispatch.rindex('pgwt_timeline_handler(')
    assert 'pgwt_timeline_clock()' in function(transport, 'linux_wait')
    assert 'attach_timeline->epoll_return_ns = n >= 0 ? d->last_epoll_return_ns : 0;' in function(daemon, 'pgwt_daemon_run')
    assert '"attach-timeline", no_argument, NULL, OPT_ATTACH_TIMELINE' in (ROOT / 'src/pg_wait_tracer.c').read_text()
    setup = function(daemon, 'pgwt_daemon_init')
    allocation = setup.index('d->attach_timeline = calloc(')
    assert setup.index('err = pg_wait_tracer_bpf__load(') < allocation < setup.index('pgwt_merge_capture_open(d)')
    assert 'return -1;' not in setup[allocation:].split('fail:', 1)[0]
    print('production callback, consume resets, epoll, handler and CLI guards: PASS')


def wakeup_checks(directory, header):
    daemon = (ROOT / 'src/daemon.c').read_text()
    cmdline = (ROOT / 'src/pg_wait_tracer.c').read_text()
    bpf = (ROOT / 'src/bpf/pg_wait_tracer.bpf.c').read_text()
    assert 'volatile const bool test_fork_wakeup_adaptive = 0;' in bpf
    option = next(line for line in cmdline.splitlines() if '{"test-fork-wakeup",' in line)
    assert 'required_argument, NULL, OPT_TEST_FORK_WAKEUP' in option
    case = cmdline[cmdline.index('        case OPT_TEST_FORK_WAKEUP:'):cmdline.index('        case OPT_SAMPLE_RATE:')]
    assignment = next(line for line in daemon.splitlines() if 'rodata->test_fork_wakeup_adaptive =' in line)
    hook = '        if (d->attach_timeline)\n            pgwt_timeline_wake(d->attach_timeline, n, d->last_epoll_return_ns);'
    setup = function(daemon, 'pgwt_daemon_init')
    assert setup.index(assignment.strip()) < setup.index('err = pg_wait_tracer_bpf__load(')
    assert hook in function(daemon, 'pgwt_daemon_run')
    assert 'if (d->attach_timeline) d->attach_timeline->wake_active = false;' in function(daemon, 'pgwt_daemon_run')
    # Real getopt row, switch branch and pre-load rodata assignment, with only
    # the skeleton allocation replaced. This checks default, both arms, bad
    # values and last-option-wins without building/loading a BPF object.
    source = r'''
#include "daemon.h"
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
struct rodata { bool test_fork_wakeup_adaptive; };
struct pg_wait_tracer_bpf { struct rodata *rodata; };
#define OPT_TEST_FORK_WAKEUP 284
static struct option options[] = { OPTION {NULL,0,NULL,0} };
static void configure(struct pgwt_daemon *d) { ASSIGNMENT }
int main(int argc, char **argv)
{
    static struct pgwt_daemon storage;
    struct pgwt_daemon *d=&storage;
    struct rodata ro={0}; struct pg_wait_tracer_bpf skel={.rodata=&ro}; d->skel=&skel;
    int c;
    while((c=getopt_long(argc,argv,"",options,NULL))!=-1) {
        switch(c) { CASE default: return 2; }
    }
    configure(d); printf("adaptive=%d\n",ro.test_fork_wakeup_adaptive); return 0;
}
'''.replace('OPTION', option).replace('ASSIGNMENT', assignment).replace('CASE', case)
    result = compile_run(directory, 'knob', source, header)
    assert result.stdout == 'adaptive=0\n', 'FAIL knob default force'
    binary = directory / 'knob'
    def assert_knob(result, code, output):
        assert (result.returncode, result.stdout) == (code, output), 'FAIL knob plumbing'
    for args, code, output in [(['--test-fork-wakeup=adaptive'], 0, 'adaptive=1\n'),
                               (['--test-fork-wakeup=force'], 0, 'adaptive=0\n'),
                               (['--test-fork-wakeup=adaptive', '--test-fork-wakeup=force'], 0, 'adaptive=0\n'),
                               (['--test-fork-wakeup=bad'], 1, '')]:
        result = subprocess.run([str(binary), *args], text=True, capture_output=True)
        assert_knob(result, code, output)
    print('knob: production getopt/switch/rodata defaults, force, adaptive, invalid and last option PASS')
    # Mutate the actual pre-load assignment, build and reach the adaptive arm.
    mutant = source.replace(assignment, 'd->skel->rodata->test_fork_wakeup_adaptive = false;')
    compile_run(directory, 'knob_ignored', mutant, header)
    result = subprocess.run([str(directory / 'knob_ignored'), '--test-fork-wakeup=adaptive'], text=True, capture_output=True)
    try:
        assert_knob(result, 0, 'adaptive=1\n')
    except AssertionError as error:
        assert str(error) == 'FAIL knob plumbing'
    else:
        raise AssertionError('FAIL ignored knob mutant survived')
    print('KILLED knob_rodata_ignored: built, reached adaptive arm, FAIL knob plumbing')
    wake_source = r'''
#include "daemon.h"
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s\n",#x);exit(1);}}while(0)
uint64_t pgwt_debug_monotonic_ns(void) {return 100;}
static void wake_hook(struct pgwt_daemon *d,int n) { HOOK }
int main(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d)); CHECK(d);
    wake_hook(d,1); /* disabled path */
    d->attach_timeline=calloc(1,sizeof(*d->attach_timeline)); CHECK(d->attach_timeline);
    struct pgwt_attach_timeline *t=d->attach_timeline;
    pgwt_timeline_dequeue(t); /* outside any outer pass */
    d->last_epoll_return_ns=100; wake_hook(d,-1); pgwt_timeline_dequeue(t);
    CHECK(t->outer_loop_wakes==0 && t->outer_loop_lifecycle_wakes==0);
    wake_hook(d,0); pgwt_timeline_dequeue(t); pgwt_timeline_dequeue(t);
    CHECK(t->outer_loop_wakes==1 && t->outer_loop_lifecycle_wakes==1);
    d->last_epoll_return_ns=200; wake_hook(d,1); /* empty return */
    d->last_epoll_return_ns=300; wake_hook(d,1);
    pgwt_timeline_dequeue(t); pgwt_timeline_dequeue(t);
    CHECK(t->outer_loop_wakes==3 && t->outer_loop_lifecycle_wakes==2);
    t->wake_active=false; pgwt_timeline_dequeue(t); /* shutdown */
    CHECK(t->outer_loop_lifecycle_wakes==2);
    pgwt_timeline_summary(stdout,t); free(t); free(d); puts("REACHED wake accounting");
    return 0;
}
'''.replace('HOOK', hook)
    baseline = compile_run(directory, 'wakes', wake_source, header)
    stats = parse(baseline.stdout)['stats'][0]
    assert stats['outer_loop_wakes'] == 3 and stats['outer_loop_lifecycle_wakes'] == 2
    assert stats['outer_loop_elapsed_ns'] == 200
    for name, before, after, intended in [
        ('wake_counts_errors', 't->wake_active = ready >= 0;', 't->wake_active = ready >= -1;',
         'FAIL t->outer_loop_wakes==0 && t->outer_loop_lifecycle_wakes==0'),
        ('wake_counts_records', 't->wake_active && !t->wake_has_lifecycle', 't->wake_active',
         'FAIL t->outer_loop_wakes==1 && t->outer_loop_lifecycle_wakes==1'),
        ('wake_never_resets', 't->wake_has_lifecycle = false;', '/* no reset */',
         'FAIL t->outer_loop_wakes==3 && t->outer_loop_lifecycle_wakes==2')]:
        assert header.count(before) == 1
        result = compile_run(directory, name, wake_source, header.replace(before, after), intended=intended)
        assert intended in result.stderr, (name, result.stderr)
        print(f'KILLED {name}: built, reached wake accounting, {intended}')
    (directory / 'attach_timeline.h').write_text(header)
    print('outer wake accounting: timeout/error/empty/multiple batches/disabled/shutdown and JSON PASS')


def paired_report_checks():
    module = ast.parse((ROOT / 'tests/init_loss_repro.py').read_text())
    nodes = [n for n in module.body if isinstance(n, ast.FunctionDef) and n.name in ('wakeup_runs', 'timeline_report')]
    namespace = {}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), '<paired report>', 'exec'), namespace)
    assert namespace['wakeup_runs'](3, True) == [(1,'force'),(1,'adaptive'),(2,'adaptive'),(2,'force'),(3,'force'),(3,'adaptive')]
    assert namespace['wakeup_runs'](2, False) == [(1,'default'),(2,'default')]
    rows = [dict(fork_ns=100, rb_receive_ns=100+d, batch_records_before=b, lost=lost, outcome=o)
            for d,b,lost,o in [(20,0,False,'attached'), (1,0,True,'never_fired'),
                               (100,2,True,'fired+stored'), (10,1,False,'attached')]]
    rows.append(dict(fork_ns=None, rb_receive_ns=None, batch_records_before=None, lost=True, outcome='unknown'))
    report = namespace['timeline_report']({'timelines':rows, 'stats':[dict(outer_loop_wakes=12, outer_loop_lifecycle_wakes=8,
                                                                        outer_loop_elapsed_ns=2000000000, pending=3)]})
    assert report['all']['fork_to_receive_ns'] == dict(n=4,p50=10,p90=100,p99=100)
    assert report['all']['backends'] == 5 and report['all']['lost'] == 3
    assert report['all']['by_outcome']['attached'] == dict(backends=2,lost=0)
    assert report['all']['by_outcome']['unknown'] == dict(backends=1,lost=1)
    assert report['batch_records_before=0']['fork_to_receive_ns'] == dict(n=2,p50=1,p90=20,p99=20)
    assert report['batch_records_before>0']['fork_to_receive_ns'] == dict(n=2,p50=10,p90=100,p99=100)
    assert report['batch_records_before_unknown'] == 1
    assert report['wakes_per_second'] == 6 and report['lifecycle_wakes_per_second'] == 4
    assert report['diagnostic_stats']['pending'] == 3
    empty = namespace['timeline_report']({'timelines':[], 'stats':[]})
    assert empty['all']['fork_to_receive_ns'] == dict(n=0,p50=None,p90=None,p99=None)
    assert empty['wakes_per_second'] is None
    print('paired harness: balanced interleaving, hand-calculated ranks/strata/loss denominators/outcomes/wake rates/nulls PASS')


def main():
    header = HEADER.read_text()
    with tempfile.TemporaryDirectory(prefix='pgwt-attach-timeline-') as tmp:
        directory = Path(tmp)
        baseline = compile_run(directory, 'timeline', SYNTHETIC, header)
        check_fields(baseline)
        check_summaries(baseline)
        print(baseline.stdout.splitlines()[0])
        print(next(line for line in baseline.stdout.splitlines() if line.startswith('ATTACH-TIMELINE-SUMMARY:')))
        print('synthetic fields, nulls, outcomes, nearest ranks and loss populations: PASS')
        reuse = compile_run(directory, 'pid_reuse', REUSE, header)
        reuse_rows = parse(reuse.stdout)
        assert len(reuse_rows['timelines']) == 2
        assert reuse_rows['timelines'][0]['fork_ns'] is None
        expected_reuse = dict(fork_ns=2000, rb_receive_ns=3, handler_entry_ns=4,
                              wait_enable_ns=5, exit_ns=3000, outcome='attached')
        for key, value in expected_reuse.items():
            assert reuse_rows['timelines'][1][key] == value, (key, value)
        assert reuse_rows['stats'][0] == dict(completed=2, pending=0, table_overflows=0,
                                             summary_dropped=0, replaced_pending=0,
                                             summary_allocation_ok=True, outer_loop_wakes=0,
                                             outer_loop_lifecycle_wakes=0, outer_loop_elapsed_ns=0,
                                             pgbs_resolve_calls=0, pgbs_resolve_misses=0,
                                             pgbs_resolve_total_ns=0,
                                             pgbs_resolve_extent_fallbacks=0)
        print('forkless EXIT then PID reuse: 2/2 EXIT lines and fields PASS')
        reset = 'r->slot == 2 || (r->fork_ns && r->fork_ns != ev->timestamp)'
        assert header.count(reset) == 1
        result = compile_run(directory, 'no_completed_reset', REUSE,
                             header.replace(reset, '(r->fork_ns && r->fork_ns != ev->timestamp)'),
                             intended='FAIL t->completed==2 && t->pending==0 && t->table_overflows==0')
        assert 'REACHED completed PID reuse after forkless EXIT' in result.stdout
        print('KILLED no_completed_reset: built, reached PID reuse, FAIL completed/pending/overflows')
        probe_point = 'struct pgwt_timeline_row *r = &t->rows[slot];'
        assert header.count(probe_point) == 1
        probe_header = header.replace(probe_point, probe_point + ' ++probes;')
        probes = compile_run(directory, 'probe_bound', PROBES, probe_header)
        assert parse(probes.stdout)['stats'][0]['table_overflows'] == 1
        for line in probes.stdout.splitlines():
            if line.startswith(('REACHED', 'ATTACH-TIMELINE-STATS:')):
                print(line)
        mutants = [
            ('swap_fork_receive_fields', 'v[0], v[1], v[2]', 'v[1], v[0], v[2]',
             check_fields, 'FAIL field fork_ns:'),
            ('wrong_median_rank', 'n, 50) : 0', 'n, 90) : 0', check_summaries, 'FAIL summary fork_to_receive_ns:'),
            ('lost_includes_everyone', '(!lost || s->lost)', '(true || s->lost)',
             check_summaries, 'FAIL summary lost ranks'),
        ]
        for name, before, after, check, intended in mutants:
            assert header.count(before) == 1, name
            result = compile_run(directory, name, SYNTHETIC, header.replace(before, after))
            assert 'REACHED timeline edges:' in result.stdout, 'mutant did not reach all cases'
            try:
                check(result)
            except AssertionError as error:
                assert str(error).startswith(intended), (name, error)
                print(f'KILLED {name}: built, reached timelines, {intended}')
            else:
                raise AssertionError(f'{name} survived')
        # Restore the header before extracting production hooks.
        (directory / 'attach_timeline.h').write_text(header)
        production_hooks(directory)
        wakeup_checks(directory, header)
        paired_report_checks()
        backend = (ROOT / 'src/backend.c').read_text()
        daemon = (ROOT / 'src/daemon.c').read_text()
        handler = 'd->init_diagnostics.receipt_cookie, '
        assert daemon.count(handler + 'true);') == daemon.count(handler + 'false);') == 1
        swapped_handler = daemon.replace(handler + 'true);', handler + 'SWAP);').replace(
            handler + 'false);', handler + 'true);').replace(handler + 'SWAP);', handler + 'false);')
        production_hooks(directory, daemon=swapped_handler,
                         intended='FAIL production handler timestamps')
        print('KILLED swap_handler_entry_exit: built, reached FORK handler, FAIL production handler timestamps')
        assert backend.count('PGWT_TL_WAIT_OPEN,') == backend.count('PGWT_TL_WAIT_ENABLE,') == 1
        swapped_wait = backend.replace('PGWT_TL_WAIT_OPEN,', 'PGWT_TL_SWAP,').replace(
            'PGWT_TL_WAIT_ENABLE,', 'PGWT_TL_WAIT_OPEN,').replace('PGWT_TL_SWAP,', 'PGWT_TL_WAIT_ENABLE,')
        production_hooks(directory, backend=swapped_wait,
                         intended='FAIL production wait open and enable timestamps')
        print('KILLED swap_wait_open_enable: built, reached wait enable, FAIL production wait open and enable timestamps')
    print('attach timeline: fields/summary/edge/production/off-path groups PASS; 10/10 mutants killed')


if __name__ == '__main__':
    main()
