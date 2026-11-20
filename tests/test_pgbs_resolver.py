#!/usr/bin/env python3
"""PG13 resolver differential, coherent re-read, shared miss cache and mutants.

Portable file-backed memory only: never attaches BPF or reads another process.
The old search is a frozen compatibility reference, not a numeric oracle.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/backend_status_layout.c'

RESOLVER = r'''
#define _GNU_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
static ssize_t observed_pread(int, void *, size_t, off_t);
static int observed_open(const char *, int, ...);
static FILE *observed_fopen(const char *, const char *);
#define pread observed_pread
#define open observed_open
#define fopen observed_fopen
#include "@SOURCE@"
#undef pread
#undef open
#undef fopen
/* Startup/discovery subprocesses are outside this file-backed resolver test. */
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{ (void)p; (void)argv; abort(); }
int pgwt_proc_open_rw(struct pgwt_proc *p, char *const argv[])
{ (void)p; (void)argv; abort(); }
int pgwt_proc_close(struct pgwt_proc *p) { (void)p; abort(); }
int pgwt_proc_close_bounded(struct pgwt_proc *p, int ms)
{ (void)p; (void)ms; abort(); }
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",message); exit(1); } } while (0)
static unsigned bulk_reads, small_reads;
static uint64_t flip_pid_addr;
static int bulk_fault; /* 1 short, 2 error, 3 EINTR once */
static unsigned gone_after = ~0u;
static int gone_kind; /* EOF, ESRCH, or EIO followed by EOF at anchor */
static uint64_t range_low, range_high;
static int fake_mem_fd=-1;
static uint64_t fake_map_low, fake_map_high;
static int observed_open(const char *path, int flags, ...)
{
    if (fake_mem_fd>=0 && strcmp(path,"/proc/42/mem")==0) return dup(fake_mem_fd);
    return open(path,flags);
}
static FILE *observed_fopen(const char *path, const char *mode)
{
    if (fake_mem_fd>=0 && strcmp(path,"/proc/42/maps")==0) {
        FILE *f=tmpfile(); CHECK(f,"fake proc maps");
        fprintf(f,"%llx-%llx rw-s 00000000 00:00 0 /SYSVfixture\n",
                (unsigned long long)fake_map_low,(unsigned long long)fake_map_high);
        rewind(f); return f;
    }
    return fopen(path,mode);
}
static ssize_t observed_pread(int fd, void *out, size_t size, off_t offset)
{
    if (bulk_reads >= gone_after) {
        if (size > 64) ++bulk_reads; else ++small_reads;
        if (gone_kind == 1 || (gone_kind == 2 && size > 64)) {
            errno = gone_kind == 1 ? ESRCH : EIO;
            return -1;
        }
        return 0;
    }
    if (size > 64) {
        ++bulk_reads;
        CHECK((uint64_t)offset >= range_low &&
              (uint64_t)offset + size <= range_high, "bulk stays inside map edges");
        CHECK(size <= 256 * 424, "bulk band stays bounded");
        if (bulk_fault == 3) { bulk_fault=0; errno=EINTR; return -1; }
        if (bulk_fault == 2) { errno=EIO; return -1; }
        if (bulk_fault == 1) return pread(fd,out,size/2,offset);
    } else ++small_reads;
    ssize_t n=pread(fd,out,size,offset);
    if (size > 64 && n > 0 && flip_pid_addr >= (uint64_t)offset &&
        flip_pid_addr + 4 <= (uint64_t)offset + (uint64_t)n) {
        uint32_t other=999;
        CHECK(pwrite(fd,&other,4,(off_t)flip_pid_addr)==4,"mutate PID after bulk read");
        flip_pid_addr=0;
    }
    return n;
}
/* Pre-change resolver loop, copied before the implementation changed. */
static uint64_t old_resolve(int mem_fd, uint64_t map_start, uint64_t map_end,
    pid_t backend_pid, const struct PgBackendStatusLayout *layout)
{
    struct pgwt_pgbs_expected expected = {.pid = backend_pid};
    struct pgwt_pgbs_snapshot snapshot;
    uint64_t found = 0;
    const uint64_t max_rows = 16384;
    for (uint64_t distance = 0; distance <= max_rows && !found; distance++) {
        for (int direction = distance == 0 ? 0 : -1;
             direction <= 1; direction += 2) {
            uint64_t delta = distance * layout->struct_size;
            uint64_t base;
            if (direction < 0) {
                if (delta > layout->status_anchor - map_start)
                    continue;
                base = layout->status_anchor - delta;
            } else {
                if (delta > map_end - layout->status_anchor)
                    continue;
                base = layout->status_anchor + delta;
            }
            if (base < map_start || base + layout->struct_size > map_end)
                continue;
            uint64_t procpid = 0;
            if (read_field_value(mem_fd, base, &layout->st_procpid,
                                 &procpid) == 0 &&
                procpid == (uint32_t)backend_pid &&
                candidate_ok(mem_fd, base, layout, &expected, NULL,
                             &snapshot)) {
                found = base;
                break;
            }
        }
    }
    return found;
}
static uint64_t row_addr(unsigned row) { return 4096 + (uint64_t)row * 424; }
static void write_row(int fd, unsigned row, uint32_t pid)
{
    unsigned char bytes[424]={0};
    uint32_t db=7, user=8, state=2;
    uint64_t activity=row_addr(40003);
    memcpy(bytes+4,&pid,4); memcpy(bytes+48,&db,4); memcpy(bytes+52,&user,4);
    memcpy(bytes+232,&state,4); memcpy(bytes+248,&activity,8);
    CHECK(pwrite(fd,bytes,sizeof(bytes),row_addr(row))==(ssize_t)sizeof(bytes),"write fake row");
}
static void compare(int fd, struct PgBackendStatusLayout *layout,
    uint64_t low, uint64_t high, uint64_t expected, const char *label)
{
    range_low=low; range_high=high;
    uint64_t old=old_resolve(fd,low,high,42,layout);
    bulk_reads=small_reads=0;
    uint64_t actual=resolve_entry_fd(fd,low,high,42,layout);
    CHECK(old==expected && actual==old,"old/new address compatibility");
    CHECK(bulk_reads<=130,"bulk reads bounded across full search");
    CHECK(expected || small_reads==0,"miss uses bulk reads only");
    printf("REACHED %s: address=%llu bulk_reads=%u coherent_field_reads=%u\n",
           label,(unsigned long long)actual,bulk_reads,small_reads);
}
int main(void)
{
    struct PgBackendStatusLayout layout;
    CHECK(pgwt_pgbs_hard_table_lookup(13,PGWT_PGBS_ARCH_X86_64,8,
          PGWT_PGBS_ABI_SYSV_LP64,&layout)==0,"PG13 layout");
    CHECK(layout.struct_size==424,"fixture stride");
    layout.activity_buffer_size=64;
    layout.status_anchor=row_addr(20000);
    FILE *file=tmpfile(); CHECK(file,"temporary fake memory"); int fd=fileno(file);
    CHECK(ftruncate(fd,row_addr(40004))==0,"size fake memory");
    char text[64]="select 1";
    CHECK(pwrite(fd,text,sizeof(text),row_addr(40003))==(ssize_t)sizeof(text),"activity fixture");
    uint64_t low=row_addr(0), high=row_addr(40003);
    unsigned hits[]={20000,19999,20001,19745,20256,4000,36000,3616,36384};
    for (unsigned i=0;i<sizeof(hits)/sizeof(hits[0]);++i) {
        write_row(fd,hits[i],42);
        compare(fd,&layout,low,high,row_addr(hits[i]),"near/far/band-edge/cap hit");
        write_row(fd,hits[i],0);
    }
    write_row(fd,3615,42); write_row(fd,36385,42);
    compare(fd,&layout,low,high,0,"outside original distance cap");
    write_row(fd,3615,0); write_row(fd,36385,0);
    compare(fd,&layout,low,high,0,"miss");
    write_row(fd,19997,42); write_row(fd,20003,42);
    compare(fd,&layout,low,high,row_addr(19997),"equal distance negative before positive");
    write_row(fd,19997,0); write_row(fd,20003,0);
    unsigned edges[]={0,40002};
    for (unsigned i=0;i<2;++i) {
        layout.status_anchor=row_addr(edges[i]); write_row(fd,edges[i],42);
        compare(fd,&layout,low,high,row_addr(edges[i]),"array edge anchor");
        write_row(fd,edges[i],0);
        unsigned other=i ? 40001 : 1; write_row(fd,other,42);
        compare(fd,&layout,low,high,row_addr(other),"array edge neighbor");
        write_row(fd,other,0);
    }
    layout.status_anchor=row_addr(20000);
    write_row(fd,19999,42);
    compare(fd,&layout,row_addr(19999)-7,row_addr(20002)+17,row_addr(19999),"unaligned mapping edges");
    write_row(fd,19999,0);
    write_row(fd,20001,42); flip_pid_addr=row_addr(20001)+4;
    range_low=low; range_high=high;
    puts("REACHED PID changes after bulk before coherent re-read");
    CHECK(resolve_entry_fd(fd,low,high,42,&layout)==0,"changed PID must not be accepted");
    CHECK(flip_pid_addr==0,"race injection reached");
    write_row(fd,20001,0);
    /* A short/error band must retain individually readable candidates. */
    for (bulk_fault=1; bulk_fault<=2; ++bulk_fault) {
        write_row(fd,20001,42);
        CHECK(resolve_entry_fd(fd,low,high,42,&layout)==row_addr(20001),"bulk failure fallback");
        write_row(fd,20001,0);
    }
    bulk_fault=3; write_row(fd,20001,42);
    CHECK(resolve_entry_fd(fd,low,high,42,&layout)==row_addr(20001),"bulk EINTR retry");
    write_row(fd,20001,0);
    puts("REACHED target mm retires during scan");
    unsigned exits[]={0,2,64};
    for (gone_kind=0;gone_kind<3;++gone_kind) {
        for (unsigned i=0;i<3;++i) {
            bulk_reads=small_reads=0; gone_after=exits[i];
            CHECK(resolve_entry_fd(fd,low,high,42,&layout)==0,"retired mm returns miss");
            CHECK(bulk_reads<=gone_after+1 && small_reads<2,
                  "retired mm ends scan without row fallback");
        }
    }
    gone_after=~0u;
    layout.status_anchor=row_addr(20000);
    puts("REACHED PG13 configuration and backend-ID derivation");
    CHECK(set_pg13_extent(&layout,61,100,3,8,10),"derive PG13 extent");
    CHECK(layout.status_array_base==row_addr(19940) && layout.status_array_slots==129,
          "PG13 configured slots include launcher and seven auxiliary types");
    range_low=layout.status_array_base;
    range_high=range_low+129*424;
    puts("REACHED every row in proved PG13 array");
    for (unsigned row=19940;row<19940+129;++row) {
        write_row(fd,row,42);
        uint64_t old=old_resolve(fd,low,high,42,&layout);
        bulk_reads=small_reads=0;
        uint64_t actual=resolve_entry_fd(fd,low,high,42,&layout);
        CHECK(actual==row_addr(row) && actual==old,"bounded/unbounded every-row identity");
        CHECK(bulk_reads<=2,"proved array scan reads only real rows");
        write_row(fd,row,0);
    }
    bulk_reads=small_reads=0;
    CHECK(old_resolve(fd,low,high,42,&layout)==0 &&
          resolve_entry_fd(fd,low,high,42,&layout)==0,"bounded/unbounded absent PID");
    CHECK(bulk_reads==2,"absent PID uses two small array bands");
    /* An otherwise plausible row outside the allocation must not be used. */
    write_row(fd,19939,42); write_row(fd,20069,42);
    CHECK(resolve_entry_fd(fd,low,high,42,&layout)==0,"exclude rows outside real array");
    write_row(fd,19939,0); write_row(fd,20069,0);
    CHECK(!set_pg13_extent(&layout,0,100,3,8,10) && !layout.status_array_slots,
          "missing backend ID never guesses base");
    CHECK(!set_pg13_extent(&layout,123,100,3,8,10),"invalid backend ID refuses extent");
    CHECK(!set_pg13_extent(&layout,1,UINT64_MAX,3,8,10),"configuration overflow refuses extent");
    range_low=low; range_high=high;
    compare(fd,&layout,low,high,0,"unknown extent retains original bound");
    CHECK(set_pg13_extent(&layout,61,100,3,8,10),"restore extent");
    layout.status_array_base=high-424;
    compare(fd,&layout,low,high,0,"extent outside mapping retains original bound");
    /* Drive the public API over fake proc files to check fallback accounting. */
    layout.validation=PGWT_PGBS_VALIDATION_VALIDATED;
    layout.st_changecount.validation=layout.st_procpid.validation=
        layout.st_databaseid.validation=layout.st_userid.validation=
        layout.st_state.validation=layout.st_activity_raw.validation=PGWT_PGBS_FIELD_VALIDATED;
    CHECK(pgwt_pgbs_sampled_activity_enabled(&layout),"public resolver enabled");
    fake_mem_fd=fd; fake_map_low=low; fake_map_high=high;
    struct pgwt_pgbs_resolve_stats stats={0};
    puts("REACHED public resolver fallback diagnostics");
    CHECK(pgwt_pgbs_resolve_entry(42,&layout,&stats)==0 && stats.extent_fallbacks==1,
          "invalid extent fallback is counted");
    CHECK(set_pg13_extent(&layout,61,100,3,8,10),"public bounded extent");
    range_low=layout.status_array_base; range_high=range_low+129*424;
    CHECK(pgwt_pgbs_resolve_entry(42,&layout,&stats)==0 && stats.extent_fallbacks==1,
          "bounded scan does not count fallback");
    layout.status_array_slots=0; range_low=low; range_high=high;
    CHECK(pgwt_pgbs_resolve_entry(42,&layout,&stats)==0 && stats.extent_fallbacks==2,
          "unknown extent fallback is counted");
    fake_mem_fd=-1;
    fclose(file); puts("resolver differential and coherent race: PASS");
    return 0;
}
'''

CACHE = r'''
#include "backend.h"
#include "backend_status_layout.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",message); exit(1); } } while (0)
static unsigned scans;
static uint64_t next_row, clock_ns=1000;
static uint64_t resolve_clock_ns(void) { errno=EIO; return clock_ns+=10; }
static uint64_t pgwt_pgbs_resolve_entry_stub(pid_t pid, const struct PgBackendStatusLayout *layout,
    struct pgwt_pgbs_resolve_stats *stats)
{ (void)stats; CHECK(pid==42 && layout,"resolver arguments"); ++scans; errno=ESRCH; return next_row; }
#define pgwt_pgbs_resolve_entry pgwt_pgbs_resolve_entry_stub
@CACHE_FUNCTION@
#undef pgwt_pgbs_resolve_entry
@READER_RESET_FUNCTION@
@ALLOC_FUNCTION@
/* Extracted production call expressions: all users share the same state. */
#include "daemon.h"
uint64_t pgwt_debug_monotonic_ns(void) { return clock_ns; }
static void query_text(struct pgwt_daemon *d, struct pgwt_backend *be) { @QUERY_CALL@ }
static void exact_seed(struct pgwt_daemon *d, struct pgwt_backend *be) { @EXACT_CALL@ }
int main(void)
{
    static struct pgwt_daemon d;
    struct pgwt_backend *be=alloc_backend(&d.backends,42,false); CHECK(be,"allocated backend");
    d.attach_timeline=calloc(1,sizeof(*d.attach_timeline)); CHECK(d.attach_timeline,"diagnostics");
    query_text(&d,be);
    puts("REACHED repeated QUERY_TEXT after first miss");
    for (unsigned i=0;i<100;++i) { query_text(&d,be); }
    CHECK(scans==1,"scans once per incarnation before backoff");
    CHECK(be->pgbs_resolve_attempts==1 && be->pgbs_resolve_retry_ns==100001000,
          "first miss backoff");
    CHECK(d.attach_timeline->pgbs_resolve.calls==1 &&
          d.attach_timeline->pgbs_resolve.misses==1 &&
          d.attach_timeline->pgbs_resolve.total_ns==10,"miss diagnostics");
    CHECK(errno==ESRCH,"diagnostic clock preserves resolver errno");
    puts("REACHED coherent preseed during fresh miss backoff");
    next_row=8192; exact_seed(&d,be);
    CHECK(scans==2 && be->pgbs_addr==8192,"preseed ignores fresh miss backoff");
    CHECK(!be->pgbs_resolve_attempts && !be->pgbs_resolve_retry_ns,
          "preseed records positive result in shared cache");
    be->pgbs_addr=0; next_row=0; query_text(&d,be);
    unsigned before_preseed=scans; exact_seed(&d,be);
    CHECK(scans==before_preseed+1 && be->pgbs_resolve_attempts==2,
          "failed forced preseed records its own miss");
    unsigned before_retry=scans;
    clock_ns=be->pgbs_resolve_retry_ns; query_text(&d,be);
    CHECK(scans==before_retry+1 && be->pgbs_resolve_attempts==3,"query text retries only after backoff");
    for (unsigned i=0;i<6;++i) {
        clock_ns=be->pgbs_resolve_retry_ns; uint64_t before=clock_ns;
        query_text(&d,be);
        uint64_t delay=UINT64_C(100000000) << (i+3<5 ? i+3 : 5);
        CHECK(be->pgbs_resolve_retry_ns==before+delay,"bounded exponential backoff");
    }
    next_row=8192; /* Even the long back-off must not deny coherent preseed. */
    exact_seed(&d,be);
    CHECK(be->pgbs_addr==8192 && !be->pgbs_resolve_attempts && !be->pgbs_resolve_retry_ns,
          "late published row succeeds and clears miss state");
    unsigned resolved=scans;
    for (unsigned i=0;i<10;++i) { query_text(&d,be); exact_seed(&d,be); }
    CHECK(scans==resolved,"positive cache shared by all callers");
    /* Actual production allocation resets miss/positive state on slot reuse. */
    be->is_alive=false; be->pid=0;
    be=alloc_backend(&d.backends,42,false); CHECK(be,"reused incarnation");
    CHECK(!be->pgbs_addr && !be->pgbs_resolve_attempts && !be->pgbs_resolve_retry_ns,
          "new incarnation clears cache");
    next_row=0; query_text(&d,be);
    CHECK(scans==resolved+1,"reused PID scans independently");
    d.attach_timeline->pgbs_resolve=(struct pgwt_pgbs_resolve_stats){
        .calls=9,.misses=8,.total_ns=12345,.extent_fallbacks=6};
    pgwt_timeline_summary(stdout,d.attach_timeline);
    free(d.attach_timeline); d.attach_timeline=NULL;
    be->pgbs_resolve_attempts=0; clock_ns=100;
    query_text(&d,be); CHECK(clock_ns==100,"disabled diagnostics add no timing calls");
    puts("shared cache, incarnation reuse, bounded retry and diagnostics: PASS");
    return 0;
}
'''


def function(source, name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\n\{.*?^\}',
                      source, re.M | re.S)
    assert match, name
    return match[0]


def cache_source(source):
    # Master replaced per-backend sampler scans with its counted batch path.
    # That path has its own syscall-budget/mutation suite; this cache is shared
    # only by QUERY_TEXT and exact preseed after the port.
    sampler = (ROOT / 'src/sampler.c').read_text()
    assert 'pgwt_pgbs_resolve_cached(' not in sampler
    assert 'pgwt_pgbs_resolve_pg13_batch(' in sampler
    result = CACHE.replace('@CACHE_FUNCTION@', function(source, 'pgwt_pgbs_resolve_cached'))
    result = result.replace('@READER_RESET_FUNCTION@', function(source, 'pgwt_pgbs_reader_reset'))
    result = result.replace('@ALLOC_FUNCTION@', function((ROOT / 'src/backend.c').read_text(), 'alloc_backend'))
    for token, file in [('QUERY', 'daemon.c'), ('EXACT', 'exact_probe.c')]:
        caller = (ROOT / 'src' / file).read_text()
        calls = re.findall(r'pgwt_pgbs_resolve_cached\(.*?;', caller, re.S)
        assert len(calls) == 1, file
        assert 'pgwt_pgbs_resolve_entry(' not in caller, file
        result = result.replace('@' + token + '_CALL@', calls[0])
    return result


def run(directory, name, source, extra=(), intended=None, reached=None):
    cfile, binary = directory / (name + '.c'), directory / name
    cfile.write_text(source)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-g', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                    *shlex.split(os.environ.get('CFLAGS', '')), str(cfile), *map(str, extra),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True,
                            env=dict(os.environ, ASAN_OPTIONS='halt_on_error=1:exitcode=86',
                                     UBSAN_OPTIONS='halt_on_error=1:exitcode=87'))
    assert not any(s in result.stderr for s in ('AddressSanitizer', 'runtime error:')), result.stderr
    if intended:
        assert result.returncode == 1 and 'FAIL ' + intended in result.stderr, result.stdout + result.stderr
        assert reached in result.stdout, result.stdout
        print(f'KILLED {name}: built, reached intended case, FAIL {intended}')
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        print('\n'.join(line for line in result.stdout.splitlines()
                        if not line.startswith('ATTACH-TIMELINE-SUMMARY:')))
    return result


def main():
    source = SOURCE.read_text()
    with tempfile.TemporaryDirectory(prefix='pgwt-pgbs-resolver-') as tmp:
        directory = Path(tmp)
        resolver = RESOLVER.replace('@SOURCE@', str(SOURCE))
        run(directory, 'resolver', resolver)
        cached = run(directory, 'cache', cache_source(source))
        stats = [json.loads(line.split(': ', 1)[1]) for line in cached.stdout.splitlines()
                 if line.startswith('ATTACH-TIMELINE-STATS:')]
        assert len(stats) == 1
        assert {k: stats[0][k] for k in ('pgbs_resolve_calls', 'pgbs_resolve_misses',
                                        'pgbs_resolve_total_ns', 'pgbs_resolve_extent_fallbacks')} == dict(
            pgbs_resolve_calls=9, pgbs_resolve_misses=8, pgbs_resolve_total_ns=12345,
            pgbs_resolve_extent_fallbacks=6)
        print('attach summary resolver calls/misses/time JSON: PASS')
        # Each mutant builds and reaches the dedicated runtime assertion.
        before = 'be->pgbs_addr || (!force && be->pgbs_resolve_attempts &&'
        assert source.count(before) == 1
        run(directory, 'drop_miss_cache', cache_source(source.replace(before, 'be->pgbs_addr || (!force && false &&')),
            intended='scans once per incarnation before backoff', reached='REACHED repeated QUERY_TEXT')
        before = 'candidate_ok(mem_fd, base, layout, &expected, NULL,\n                                 &snapshot)'
        assert source.count(before) == 1
        # Keep coherent read executable but ignore its identity verdict.
        mutant = source.replace(before, '(' + before + ', true)')
        shadow = directory / 'mutant.c'; shadow.write_text(mutant)
        run(directory, 'drop_coherent_verdict', RESOLVER.replace('@SOURCE@', str(shadow)),
            intended='changed PID must not be accepted',
            reached='REACHED PID changes after bulk')
        before = 'if (gone) goto done;'
        assert source.count(before) == 2
        shadow.write_text(source.replace(before, 'if (gone) { /* mutant ignores retirement */ }'))
        run(directory, 'ignore_mm_retirement', RESOLVER.replace('@SOURCE@', str(shadow)),
            intended='retired mm ends scan without row fallback',
            reached='REACHED target mm retires during scan')
        before = '!force && be->pgbs_resolve_attempts'
        assert source.count(before) == 1
        # Keep force evaluated so the mutant compiles under -Werror.
        mutant = source.replace(before, '(force || !force) && be->pgbs_resolve_attempts')
        run(directory, 'preseed_obeys_backoff', cache_source(mutant),
            intended='preseed ignores fresh miss backoff',
            reached='REACHED coherent preseed during fresh miss backoff')
        before = 'bool bounded = status_array_bounds('
        assert source.count(before) == 1
        shadow.write_text(source.replace(before, 'bool bounded = false && status_array_bounds('))
        run(directory, 'drop_array_extent', RESOLVER.replace('@SOURCE@', str(shadow)),
            intended='bulk stays inside map edges', reached='REACHED every row in proved PG13 array')
        before = 'const uint64_t aux_types = 7;'
        assert source.count(before) == 1
        shadow.write_text(source.replace(before, 'const uint64_t aux_types = 6;'))
        run(directory, 'wrong_auxiliary_count', RESOLVER.replace('@SOURCE@', str(shadow)),
            intended='PG13 configured slots include launcher and seven auxiliary types',
            reached='REACHED PG13 configuration and backend-ID derivation')
        before = '++stats->extent_fallbacks;'
        assert source.count(before) == 1
        shadow.write_text(source.replace(before, '(void)stats;'))
        run(directory, 'drop_extent_fallback_counter', RESOLVER.replace('@SOURCE@', str(shadow)),
            intended='invalid extent fallback is counted',
            reached='REACHED public resolver fallback diagnostics')
    print('PG13 resolver: compatibility/cache/race/edges/retirement/preseed/extent PASS; 7/7 mutants killed')


if __name__ == '__main__':
    main()
