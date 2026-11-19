#!/usr/bin/env python3
"""INIT loss branches through production receipt, EXIT and logging functions.

No BPF/kernel operation: the Linux receipt callback is compiled with fake
capture and map I/O. Mutants must build, reach the case and fail its field
assertion. All generated files live outside the repository.
"""
import ast
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|void|struct pgwt_backend \*) " + name +
                      r"\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match[0]


HARNESS = r'''
#include "daemon.h"
#include "backend_init.h"
#include "counter_vector.h"
#include <errno.h>
#include <stdlib.h>
#include <assert.h>

#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", msg); exit(1); } } while (0)
struct pg_wait_tracer_bpf { struct { void *state_map, *exact_attr_map, *exact_seed_map; } maps; };
struct linux_transport { struct pgwt_daemon *d; };
static struct pgwt_daemon d;
static struct pgwt_pid_state stored;
static int read_error, read_count, sweep_exit, deletes, clock_reads, second_read_error, raw_disposition;
static uint64_t now = 400;
static int process_lifecycle_event(void *, void *, size_t);
uint64_t pgwt_debug_monotonic_ns(void) { ++clock_reads; return now; }
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t ts)
{ (void)op; (void)pid; (void)ts; }
static void pgwt_close_watchpoint(int fd) { (void)fd; }
static int bpf_map__fd(void *p) { (void)p; return 1; }
static int bpf_map_delete_elem(int fd, const void *key)
{ (void)fd; (void)key; ++deletes; return 0; }
void pgwt_summary_forget_open(struct pgwt_summary_writer *w, uint32_t pid)
{ (void)w; (void)pid; }
void pgwt_live_qattr_pid_exit(struct pgwt_accumulator *a, uint32_t pid)
{ (void)a; (void)pid; }
const char *pgwt_backend_type_name(enum pgwt_backend_type t) { (void)t; return "client"; }
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{ return bt->entries[0].pid == pid ? &bt->entries[0] : NULL; }
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *ctx, uint32_t key)
{ (void)ctx; (void)key; return (struct pgwt_counter_value){.value=0, .valid=true}; }
void pgwt_pgbs_reader_reset(struct pgwt_pgbs_reader *reader)
{ memset(reader, 0, sizeof(*reader)); }
static int read_init_state(void *ctx, pid_t pid, struct pgwt_pid_state *out)
{ CHECK(ctx == &d && pid == 42, "map target"); ++read_count; *out = stored; return second_read_error && read_count == 2 ? -EIO : read_error; }

@FUNCTIONS@

int pgwt_sweep_bootstrap_backends(struct pgwt_daemon *ctx)
{
    if (sweep_exit) {
        sweep_exit = 0;
        /* An EXIT is received, then the PRE-record sweep retires its pid. */
        CHECK(ctx->lifecycle_exit_pid == 42, "EXIT receipt before sweep");
        pgwt_handle_exit(ctx, 42);
    }
    return 0;
}
static int process_lifecycle_event(void *ctx, void *data, size_t n)
{ (void)ctx; (void)data; (void)n; return 0; }
int pgwt_capture_received_token(struct pgwt_merge_capture *capture,
    const ma_envelope *e, uint64_t at, size_t *token)
{
    (void)capture; (void)at; *token = 0;
    if (e->kind == MA_LIFECYCLE) {
        struct pgwt_lifecycle_event ev; memcpy(&ev, e->bytes, sizeof(ev));
        return pgwt_process_lifecycle_payload(&d, &ev, sizeof(ev));
    }
    return raw_disposition;
}
static void receipt(int type, uint64_t ts, uint64_t cookie)
{
    struct linux_transport l = {.d = &d};
    struct pgwt_lifecycle_event ev = {.type = type, .pid = 42, .timestamp = ts};
    ma_envelope e = {.kind = MA_LIFECYCLE, .cookie = cookie, .bytes = (void *)&ev, .size = sizeof(ev)};
    size_t token;
    CHECK(linux_received(&l, &e, ts + 10, &token) == 0, "lifecycle received");
    CHECK(d.init_diagnostics.receipt_cookie == 0, "receipt cookie scope restored");
}
static void exit_disposition(uint64_t cookie, uint16_t flags, uint64_t init_ts)
{
    struct linux_transport l = {.d = &d};
    struct pgwt_lifecycle_event ev = {.type=PGWT_LIFECYCLE_EXIT, .pid=42,
        .timestamp=300, .addr=init_ts, .exit_flags=flags};
    ma_envelope e = {.kind=MA_LIFECYCLE, .cookie=cookie, .bytes=(void *)&ev, .size=sizeof(ev)};
    size_t token;
    CHECK(linux_received(&l, &e, 310, &token) == 0, "EXIT disposition received");
}
static void trace(uint64_t cookie, uint32_t old_event)
{
    struct linux_transport l = {.d = &d};
    struct pgwt_trace_event ev = {.pid = 42, .old_event = old_event};
    ma_envelope e = {.kind = MA_TRACE, .cookie = cookie, .bytes = (void *)&ev, .size = sizeof(ev)};
    size_t token;
    CHECK(linux_received(&l, &e, 250, &token) == raw_disposition, "trace received");
}
static void reset(void)
{
    static struct pg_wait_tracer_bpf skel;
    memset(&d, 0, sizeof(d)); d.skel = &skel; d.exact_perf = true;
    d.backends.entries[0] = (struct pgwt_backend){.pid=42, .bootstrap_fd=88,
        .wp_fd=-1, .is_alive=true, .init_diag_cookie=7};
    read_error = read_count = sweep_exit = deletes = clock_reads = second_read_error = raw_disposition = 0;
    stored = (struct pgwt_pid_state){.init_stored=1, .init_ts=123};
    d.init_last_sweep_start_ns=200;
}
int main(void)
{
    reset(); receipt(PGWT_LIFECYCLE_FORK, 100, 7); trace(7, 0);
    raw_disposition=2; trace(7, 1); raw_disposition=0; /* counted even if capture discards */
    trace(7, PGWT_MARKER_CMD_START); /* marker, never a wait transition */
    CHECK(read_count == 0 && clock_reads == 0, "receipt tracking adds no map or clock reads");
    sweep_exit=1; receipt(PGWT_LIFECYCLE_EXIT, 300, 7);
    CHECK(read_count == 2 && d.counters.lifecycle_init_lost_total == 1, "state_present decision");
    CHECK(!pgwt_init_diag_find(&d.init_diagnostics,42,7,false), "EXIT retires history");
    puts("REACHED state_present");

    reset(); read_error=-ENOENT; receipt(PGWT_LIFECYCLE_FORK,100,7);
    trace(7,0); sweep_exit=1; receipt(PGWT_LIFECYCLE_EXIT,300,7);
    CHECK(read_count == 1 && d.counters.lifecycle_init_lost_total == 1, "missing_bootstrap decision");
    puts("REACHED state_missing_bootstrap");

    reset(); read_error=-ENOENT; d.backends.entries[0].bootstrap_fd=-1;
    d.backends.entries[0].init_ts=123;
    receipt(PGWT_LIFECYCLE_FORK,100,7); receipt(PGWT_LIFECYCLE_INIT,123,7); trace(7,0);
    pgwt_handle_exit(&d,42); /* liveness observation: EXIT not yet received */
    CHECK(read_count == 1 && d.counters.lifecycle_init_lost_total == 1, "missing_hint decision");
    puts("REACHED state_missing_init_ts");

    /* Other lifetime records must not alter the lost backend's evidence. */
    reset(); read_error=-ENOENT; receipt(PGWT_LIFECYCLE_FORK,100,7);
    receipt(PGWT_LIFECYCLE_FORK,101,8); trace(8,0); trace(8,0); trace(7,0);
    receipt(PGWT_LIFECYCLE_INIT,124,8);
    pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 1, "cookie isolation loss");
    puts("REACHED cookie_isolation");

    reset(); read_error=-ENOENT; receipt(PGWT_LIFECYCLE_FORK,100,7);
    struct linux_transport l = {.d=&d};
    struct pgwt_trace_event exit_trace={.pid=42,.new_event=PGWT_EVENT_EXIT,.timestamp_ns=300};
    ma_envelope e={.kind=MA_TRACE,.cookie=7,.bytes=(void *)&exit_trace,.size=sizeof(exit_trace)};
    size_t token;
    CHECK(linux_received(&l,&e,310,&token) == 0, "EXIT transition received");
    pgwt_handle_exit(&d,42);
    puts("REACHED exit_transition_time");

    reset(); receipt(PGWT_LIFECYCLE_FORK,100,7);
    pgwt_init_diag_attached(&d.init_diagnostics,42,7); trace(7,0);
    CHECK(pgwt_init_diag_find(&d.init_diagnostics,42,7,false)->transitions == 0, "attached fast path");
    d.backends.entries[0].init_attached=true; pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 0, "attached is not lost");
    reset(); read_error=-EIO; pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 0, "unreadable is not lost");
    reset(); read_error=-ENOENT; d.backends.entries[0].bootstrap_fd=-1; pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 0, "absent without evidence is not lost");
    reset(); stored=(struct pgwt_pid_state){0}; pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 0, "present without INIT is not lost");
    reset(); second_read_error=1; pgwt_handle_exit(&d,42);
    CHECK(d.counters.lifecycle_init_lost_total == 0 && read_count == 2, "second read error is not lost");
    puts("REACHED unchanged_nonloss_decisions");

    /* Default wire: receipt times are unknown, FORK resets reused pid. */
    reset(); d.exact_perf=false;
    struct pgwt_lifecycle_event fork={.pid=42,.type=PGWT_LIFECYCLE_FORK,.timestamp=100};
    CHECK(pgwt_process_lifecycle_payload(&d,&fork,sizeof(fork)) == 0, "default FORK");
    struct pgwt_trace_event trans={.pid=42};
    pgwt_init_diag_trace(&d.init_diagnostics,0,&trans);
    fork.timestamp=200;
    CHECK(pgwt_process_lifecycle_payload(&d,&fork,sizeof(fork)) == 0, "default reused FORK");
    struct pgwt_init_receipts *r=pgwt_init_diag_find(&d.init_diagnostics,42,0,false);
    CHECK(r && r->fork_ts == 200 && r->fork_received_ns == 0 && r->transitions == 0, "default PID reuse resets history");

    /* Three lifetime dispositions, both before and after loss. Carried
     * timestamps are reporting only: all missing-state losses retain the
     * original init_ts=0 with-opportunity accounting. */
    for (int which=0; which<3; ++which) {
        uint16_t flags=PGWT_EXIT_DISPOSITION;
        uint64_t init_ts=0;
        if (which<2) flags |= PGWT_EXIT_BOOTSTRAP_FIRED;
        if (which==0) { flags |= PGWT_EXIT_INIT_STORED; init_ts=123; }
        reset(); read_error=-ENOENT; receipt(PGWT_LIFECYCLE_FORK,100,7);
        sweep_exit=1; exit_disposition(7,flags,init_ts);
        CHECK(d.counters.lifecycle_init_lost_total==1 &&
            d.counters.lifecycle_init_lost_with_opportunity_total==1 &&
            d.counters.lifecycle_init_lost_no_opportunity_total==0 &&
            d.counters.lifecycle_init_lost_untraced_ns_total==0,
            "carried disposition leaves accounting unchanged");
        printf("REACHED disposition_before_%d\n",which);

        reset(); read_error=-ENOENT; receipt(PGWT_LIFECYCLE_FORK,100,7);
        pgwt_handle_exit(&d,42);
        /* An EXIT for a reused PID cannot consume the old lifetime join. */
        exit_disposition(8,PGWT_EXIT_DISPOSITION|PGWT_EXIT_WP_EVER_LIVE,0);
        CHECK(pgwt_init_diag_find(&d.init_diagnostics,42,7,false)->loss_pending,
            "replacement cookie cannot complete pending loss");
        exit_disposition(7,flags,init_ts);
        exit_disposition(7,flags,init_ts); /* duplicate: no second follow-up */
        CHECK(d.counters.lifecycle_init_lost_total==1,
            "late and duplicate EXIT cannot recount loss");
        printf("REACHED disposition_after_%d\n",which);
    }

    /* Loss before any receipt still retains the cookie for its late join. */
    reset(); read_error=-ENOENT; pgwt_handle_exit(&d,42);
    exit_disposition(7,PGWT_EXIT_DISPOSITION|PGWT_EXIT_BOOTSTRAP_FIRED|
        PGWT_EXIT_INIT_STORED|PGWT_EXIT_WP_EVER_LIVE,123);
    puts("REACHED disposition_no_prior_receipt");

    /* Full table, collision-chain deletion, bounded unknown and PID reuse. */
    reset();
    for (uint32_t i=0;i<PGWT_INIT_DIAG_SLOTS;++i)
        CHECK(pgwt_init_diag_find(&d.init_diagnostics,42+i*PGWT_INIT_DIAG_SLOTS,7,true), "fill bounded table");
    CHECK(!pgwt_init_diag_find(&d.init_diagnostics,999999,7,true), "capacity bounded");
    pgwt_init_diag_forget(&d.init_diagnostics,42,7);
    for (uint32_t i=0;i<PGWT_INIT_DIAG_SLOTS;++i)
        CHECK((pgwt_init_diag_find(&d.init_diagnostics,42+i*PGWT_INIT_DIAG_SLOTS,7,false)!=NULL)==(i!=0), "deletion preserves chain");
    CHECK(pgwt_init_diag_find(&d.init_diagnostics,999999,7,true), "reuse freed slot");
    pgwt_init_diag_print(stderr,&d.init_diagnostics,999999,7,"capacity_example",400);
    puts("REACHED capacity_and_delete");
    fprintf(stdout,"diagnostic_storage_bytes=%zu slots=%u\n",sizeof(d.init_diagnostics),PGWT_INIT_DIAG_SLOTS);
    return 0;
}
'''


def check_fields(result):
    assert result.returncode == 0, result.stderr
    lines = [line for line in result.stderr.splitlines() if line.startswith('INIT-LOST-EVIDENCE:')]
    assert len(lines) == 13, lines
    expected = [
        dict(branch='state_present', fork='delivered', fork_ts='100', fork_received_ns='110',
             init_record='not_received', init_record_ts='0', init_received_ns='0',
             exit_record='delivered', exit_record_ts='300', exit_received_ns='310',
             transitions_received='2', exit_to_read_ns='100', history='tracked'),
        dict(branch='state_missing_bootstrap', fork='delivered', fork_ts='100', fork_received_ns='110',
             init_record='not_received', init_record_ts='0', init_received_ns='0',
             exit_record='delivered', exit_record_ts='300', exit_received_ns='310',
             transitions_received='1', exit_to_read_ns='100', history='tracked'),
        dict(branch='state_missing_init_ts', fork='delivered', fork_ts='100', fork_received_ns='110',
             init_record='delivered', init_record_ts='123', init_received_ns='133',
             exit_record='not_received', exit_record_ts='0', exit_received_ns='0',
             transitions_received='1', exit_to_read_ns='unknown', history='tracked'),
        dict(branch='state_missing_bootstrap', fork='delivered', fork_ts='100', fork_received_ns='110',
             init_record='not_received', init_record_ts='0', init_received_ns='0',
             exit_record='not_received', exit_record_ts='0', exit_received_ns='0',
             transitions_received='1', exit_to_read_ns='unknown', history='tracked'),
        dict(branch='state_missing_bootstrap', fork='delivered', fork_ts='100', fork_received_ns='110',
             init_record='not_received', init_record_ts='0', init_received_ns='0',
             exit_record='not_received', exit_record_ts='0', exit_received_ns='0',
             transitions_received='1', exit_to_read_ns='100', history='tracked',
             bpf_exit_ts='300', bpf_exit_source='transition'),
    ]
    for line, wanted in zip(lines, expected):
        fields = dict(re.findall(r'(\w+)=(\S+)', line))
        wanted.setdefault('bpf_exit_ts', wanted['exit_record_ts'])
        wanted.setdefault('bpf_exit_source', 'lifecycle' if wanted['exit_record']=='delivered' else 'unknown')
        wanted.update(clock='CLOCK_MONOTONIC', pid='42', cookie='7', identity='cookie',
                      state_read_after_ns='400', history_overflows='0')
        for name, value in wanted.items():
            assert fields[name] == value, f'FAIL diagnostic {name}: {fields[name]} != {value}'
    assert 'history=incomplete' in lines[-1] and 'transitions_received=unknown' in lines[-1]
    for which in range(3):
        before = dict(re.findall(r'(\w+)=(\S+)', lines[5+2*which]))
        pending = dict(re.findall(r'(\w+)=(\S+)', lines[6+2*which]))
        wanted = dict(exit_bootstrap_fired=str(int(which < 2)),
                      exit_init_stored=str(int(which == 0)),
                      exit_init_ts='123' if which == 0 else '0', exit_wp_ever_live='0')
        for name, value in wanted.items():
            assert before[name] == value, f'FAIL diagnostic {name}: {before[name]} != {value}'
            assert pending[name] == 'pending', f'FAIL diagnostic {name}: expected pending'
    followups = [line for line in result.stderr.splitlines()
                 if line.startswith('INIT-LOST-EVIDENCE-EXIT:')]
    assert len(followups) == 4, f'FAIL follow-up count: {followups}'
    for which, line in enumerate(followups):
        fields = dict(re.findall(r'(\w+)=(\S+)', line))
        stored = which in (0, 3)
        wanted = dict(pid='42', cookie='7', exit_record_ts='300', exit_received_ns='310',
                      exit_bootstrap_fired=str(int(which != 2)),
                      exit_init_stored=str(int(stored)),
                      exit_init_ts='123' if stored else '0',
                      exit_wp_ever_live=str(int(which == 3)))
        for name, value in wanted.items():
            assert fields[name] == value, f'FAIL diagnostic {name}: {fields[name]} != {value}'
    assert 'history=incomplete' in lines[-2]  # no receipt before loss
    return lines, followups


def reproduction_summary_cases():
    # Compile only the actual pure parser, never run/import the Linux harness.
    source = (ROOT / 'tests/init_loss_repro.py').read_text()
    node = next(node for node in ast.parse(source).body
                if isinstance(node, ast.FunctionDef) and node.name == 'summary')
    namespace = {}
    exec(compile(ast.Module(body=[node], type_ignores=[]), '<repro summary>', 'exec'), namespace)
    summary = namespace['summary']
    for text, lost, evidence, ended in [
        ('', 0, 0, False),
        ('INIT-LOST: pid=42\nINIT-LOST-EVIDENCE: branch=state_missing_bootstrap\n'
         'exact-perf: suspended: counter_change at 400; automatic REARM disabled\n', 1, 1, True),
        ('exact-perf: suspended: allocation_fail at 400; automatic REARM pending fresh proofs\n', 0, 0, False),
        ('exact-perf: finish: stopped with N records unreleased\n'
         'exact-perf: suspended: unknown_evidence at 400; automatic REARM disabled\n', 0, 0, False),
    ]:
        assert summary(text) == dict(init_losses=lost, evidence_lines=evidence,
                                    capture_ended_counter_change=ended)
    print('reproduction log parser: 4/4 PASS (capacity and shutdown VOID stay separate)')


def main():
    reproduction_summary_cases()
    backend = (ROOT / 'src/backend.c').read_text()
    daemon = (ROOT / 'src/daemon.c').read_text()
    transport = (ROOT / 'src/percpu_transport.c').read_text()
    functions = '\n'.join([
        function(backend, 'enrollment_failure_counted'),
        function(backend, 'forget_enrollment_refusal'),
        function(backend, 'record_init_loss'), function(backend, 'pgwt_handle_exit'),
        function(backend, 'pgwt_handle_exit_at'), function(daemon, 'pgwt_process_lifecycle_payload'),
        function(transport, 'linux_received')])
    source = HARNESS.replace('@FUNCTIONS@', functions)
    with tempfile.TemporaryDirectory(prefix='pgwt-init-loss-diagnostic-') as tmp:
        directory = Path(tmp)
        header = (ROOT / 'src/init_loss_diag.h').read_text()
        for name in ('baseline', 'wrong_branch_label', 'drop_carried_init_ts', 'omit_late_exit_join'):
            # Shadow only daemon.h so its quoted include selects the mutant.
            (directory / 'daemon.h').write_text((ROOT / 'src/daemon.h').read_text())
            changed = header
            if name == 'wrong_branch_label':
                needle = '        branch, r && r->fork_ts'
                assert header.count(needle) == 1
                changed = header.replace(needle, '        "wrong_branch", r && r->fork_ts')
                # Still use the argument: a build warning is not a kill.
                changed = changed.replace('    bool known = r && !d->overflows;',
                                          '    (void)branch;\n    bool known = r && !d->overflows;')
            if name == 'drop_carried_init_ts':
                changed = header.replace('r->exit_init_ts = ev->addr;', 'r->exit_init_ts = 0;')
            if name == 'omit_late_exit_join':
                changed = header.replace('if (r->loss_pending) {', 'if (false && r->loss_pending) {')
            assert name == 'baseline' or changed != header, name
            (directory / 'init_loss_diag.h').write_text(changed)
            cfile, binary = directory / 'diagnostic.c', directory / name
            cfile.write_text(source)
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(directory), '-I' + str(ROOT / 'src'),
                       *shlex.split(os.environ.get('CFLAGS', '')), str(cfile),
                       str(ROOT / 'src/backend_init.c'), '-o', str(binary)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name != 'baseline':
                assert result.returncode == 0 and 'REACHED state_missing_init_ts' in result.stdout, result.stderr
                try:
                    check_fields(result)
                except AssertionError as error:
                    prefix = {'wrong_branch_label': 'FAIL diagnostic branch:',
                              'drop_carried_init_ts': 'FAIL diagnostic exit_init_ts:',
                              'omit_late_exit_join': 'FAIL follow-up count:'}[name]
                    assert str(error).startswith(prefix), error
                    print(f'{name}: KILLED (built, reached loss, failed {prefix})')
                else:
                    raise AssertionError(f'{name} survived')
            else:
                lines, followups = check_fields(result)
                print(result.stdout, end='')
                print('\n'.join(lines[:3] + lines[5:-1] + followups))
    print('INIT loss diagnostics: 14/14 host groups PASS; 3/3 mutants killed')


if __name__ == '__main__':
    main()
