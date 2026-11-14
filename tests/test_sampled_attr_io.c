/* Inject libc and process_vm_readv into the production translation unit.
 * Runs on macOS: no /proc, PostgreSQL, BPF, timing, or kernel assumptions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "pg_wait_tracer.h"
static FILE *io_fopen(const char *, const char *);
static int io_open(const char *, int, ...);
static int io_openat(int, const char *, int, ...);
static int io_close(int);
static ssize_t io_pread(int, void *, size_t, off_t);
static ssize_t io_readv(pid_t, const struct iovec *, unsigned long,
                        const struct iovec *, unsigned long, unsigned long);
#define fopen io_fopen
#define open io_open
#define openat io_openat
#define close io_close
#define pread io_pread
#define process_vm_readv io_readv
#define PGWT_PGBS_INJECT_VM_READV 1
#ifndef PGWT_PGBS_SOURCE
#define PGWT_PGBS_SOURCE "../src/backend_status_layout.c"
#endif
#include PGWT_PGBS_SOURCE
#undef fopen
#undef open
#undef openat
#undef close
#undef pread
#undef process_vm_readv

static int checks, failures, opens, openats, closes, reads, calls, fopens;
static int private_target, private_reader, incarnation = 1;
static int tear_row = -1, transient_tear, short_row = -1, fault_row = -1;
static int reader_gone, mutate_on_span, max_iovs, after_copy[1024], visits[1024];
static uint8_t storage[1024 * 440];
static uint64_t entries[1024];
static char activity[1024][64];
static struct PgBackendStatusLayout layout;
static struct pgwt_pgbs_reader cache[1024];
static struct pgwt_pgbs_batch_target targets[1024];
static size_t population;
static uint64_t tick_failures;
static uint8_t *row_at(size_t i) { return storage + i * layout.struct_size; }
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL: %s (open=%d openat=%d pread=%d close=%d vm=%d)\n", \
            msg, opens, openats, reads, closes, calls); } } while (0)

static FILE *io_fopen(const char *path, const char *mode)
{
    fopens++;
    if (!strstr(path, "/stat") && !strstr(path, "/maps")) return fopen(path, mode);
    FILE *fp = tmpfile();
    if (!fp) abort();
    if (strstr(path, "/stat")) {
        fputs("1000 (postgres) S", fp);
        for (int field = 4; field < 22; field++) fputs(" 0", fp);
        fprintf(fp, " %d\n", incarnation);
    } else {
        int pid = 0;
        (void)sscanf(path, "/proc/%d/maps", &pid);
        const char *perms = (pid == 42 ? private_reader : private_target) ? "rw-p" : "rw-s";
        fprintf(fp, "%llx-%llx %s 00000000 00:00 1\n",
            (unsigned long long)(uintptr_t)storage,
            (unsigned long long)(uintptr_t)(storage + population * layout.struct_size), perms);
        fprintf(fp, "%llx-%llx %s 00000000 00:00 2\n",
            (unsigned long long)(uintptr_t)activity,
            (unsigned long long)(uintptr_t)(activity + population), perms);
    }
    rewind(fp);
    return fp;
}
static int io_open(const char *path, int flags, ...)
{ (void)path; (void)flags; opens++; return 101; }
static int io_openat(int fd, const char *path, int flags, ...)
{ (void)fd; (void)path; (void)flags; openats++; return 101; }
static int io_close(int fd) { (void)fd; closes++; return 0; }
static ssize_t io_pread(int fd, void *buf, size_t size, off_t offset)
{
    (void)fd; reads++;
    uintptr_t addr = (uintptr_t)offset;
    if ((addr >= (uintptr_t)storage && addr + size <= (uintptr_t)(storage + sizeof(storage))) ||
        (addr >= (uintptr_t)entries && addr + size <= (uintptr_t)(entries + 1024)) ||
        (addr >= (uintptr_t)activity && addr + size <= (uintptr_t)(activity + 1024))) {
        memcpy(buf, (void *)addr, size); return (ssize_t)size;
    }
    errno = EFAULT; return -1;
}
static void put(size_t i, const struct pgwt_pgbs_field *field, uint64_t value)
{ memcpy(row_at(i) + field->offset, &value, field->width); }
static ssize_t io_readv(pid_t pid, const struct iovec *local, unsigned long nl,
                        const struct iovec *remote, unsigned long nr, unsigned long flags)
{
    calls++;
    CHECK(pid == 42 && flags == 0 && nl == nr, "postmaster is the only reader");
    if ((int)nr > max_iovs) max_iovs = (int)nr;
    if (nr > 1024) { errno = EINVAL; return -1; }
    if (reader_gone) { errno = ESRCH; return -1; }
    size_t total = 0;
    for (size_t j = 0; j < nr; j++) {
        uintptr_t addr = (uintptr_t)remote[j].iov_base;
        size_t len = remote[j].iov_len;
        CHECK(len == local[j].iov_len, "matching local and remote ranges");
        if (addr >= (uintptr_t)storage && addr < (uintptr_t)(storage + sizeof(storage))) {
            size_t i = (addr - (uintptr_t)storage) / layout.struct_size;
            uintptr_t offset = addr - (uintptr_t)row_at(i);
            /* Keep round one's guard: unused row padding is unreadable.
             * A whole-struct copy must not replace the required field span. */
            const struct pgwt_pgbs_field *last = layout.major == 13
                ? &layout.st_activity_raw : &layout.st_query_id;
            if (offset + len > last->offset + last->width) {
                if (total) return (ssize_t)total;
                errno = EFAULT; return -1;
            }
            if ((int)i == fault_row || ((int)i == short_row && len > 4)) {
                if (total) return (ssize_t)(total + ((int)i == short_row ? len - 1 : 0));
                errno = EFAULT; return -1;
            }
            if (offset == layout.st_changecount.offset && len == 4) {
                visits[i]++;
                uint32_t value;
                memcpy(&value, (void *)addr, 4);
                if ((int)i == tear_row && (!transient_tear || after_copy[i] < 2)) {
                    value += 2;
                    memcpy((void *)addr, &value, 4);
                    after_copy[i]++;
                }
            } else if (len > 4 && mutate_on_span) {
                /* A real writer begins/finishes while payload is copied. A
                 * misplaced counter-after before the span cannot catch it. */
                uint32_t value;
                memcpy(&value, row_at(i) + layout.st_changecount.offset, 4);
                put(i, &layout.st_changecount, value + 2);
            }
        }
        memcpy(local[j].iov_base, remote[j].iov_base, len);
        total += len;
    }
    return (ssize_t)total;
}
static void reset_counts(void)
{
    opens = openats = closes = reads = calls = max_iovs = 0;
    tick_failures = 0; fopens = 0;
    memset(visits, 0, sizeof(visits)); memset(after_copy, 0, sizeof(after_copy));
}
static void fixture(int major, size_t n)
{
    population = n;
    CHECK(pgwt_pgbs_hard_table_lookup(major, PGWT_PGBS_ARCH_X86_64, 8,
          PGWT_PGBS_ABI_SYSV_LP64, &layout) == 0, "fixture layout exists");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    struct pgwt_pgbs_field *fields[] = {&layout.st_changecount, &layout.st_procpid,
        &layout.st_databaseid, &layout.st_userid, &layout.st_state,
        &layout.st_activity_raw, &layout.st_query_id};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (fields[i]->present) fields[i]->validation = PGWT_PGBS_FIELD_VALIDATED;
    memset(storage, 0, sizeof(storage)); memset(targets, 0, sizeof(targets));
    layout.activity_buffer_size = sizeof(activity[0]);
    layout.status_anchor = (uintptr_t)storage;
    for (size_t i = 0; i < n; i++) {
        snprintf(activity[i], sizeof(activity[i]), "SELECT %zu", i);
        entries[i] = (uintptr_t)row_at(i);
        put(i, &layout.st_changecount, 20);
        put(i, &layout.st_procpid, 1000 + i);
        put(i, &layout.st_databaseid, 16384);
        put(i, &layout.st_userid, 10);
        put(i, &layout.st_state, layout.state_running);
        put(i, &layout.st_activity_raw, (uintptr_t)activity[i]);
        if (major != 13) put(i, &layout.st_query_id, 0xf123456789abcdefULL + i);
        /* Budget fixture starts AFTER enrollment. Separate tests exercise
         * the production own-pid resolver and both mapping proofs. */
        cache[i] = (struct pgwt_pgbs_reader){.pid = (pid_t)(1000 + i), .start_time = 1,
            .status_addr = entries[i], .activity_addr = (uintptr_t)activity[i],
            .shared_reader_pid = 42, .shared = true};
        targets[i].pid = (pid_t)(1000 + i); targets[i].reader = &cache[i];
        targets[i].rc = -1;
    }
    private_target = private_reader = reader_gone = mutate_on_span = transient_tear = 0;
    incarnation = 1; tear_row = short_row = fault_row = -1;
    reset_counts();
}
static void batch(void) { pgwt_pgbs_read_batch(42, &layout, targets, population, false, &tick_failures); }
static void test_budget(int major, size_t n, int expected)
{
    fixture(major, n);
    for (int tick = 0; tick < 500; tick++) {
        batch();
        for (size_t i = 0; i < n; i++)
            CHECK(targets[i].rc == 0 && targets[i].attr.cmd_open &&
                  targets[i].attr.databaseid == 16384 && targets[i].attr.userid == 10 &&
                  (major == 13 ? strcmp(targets[i].attr.activity, activity[i]) == 0 :
                   targets[i].attr.query_id == 0xf123456789abcdefULL + i),
                  "every budget row retains its context and query");
    }
    CHECK(calls == 500 * expected && max_iovs <= 1024 &&
          !opens && !openats && !reads && !closes && !fopens, "batched syscall budget over 500 ticks");
    printf("PG%d N=%zu ticks=500: vm/tick=%d expected=%d open=0 pread=0 close=0\n",
           major, n, calls/500, expected);
}
static void test_faults(int major)
{
    fixture(major, 5);
    tear_row = 2; batch();
    CHECK(targets[2].rc == -1 && !targets[2].attr.cmd_open && calls == 3 && visits[2] == 6 && tick_failures == 1,
          "persistent tear fails once after two retry calls");
    CHECK(targets[0].rc == 0 && targets[4].rc == 0 && visits[0] == 2 && visits[4] == 2,
          "only the torn row is retried");
    fixture(major, 5); tear_row = 2; transient_tear = 1; batch();
    CHECK(targets[2].rc == 0 && calls == 2, "transient tear recovers coherently");
    fixture(major, 5); short_row = 2; batch();
    CHECK(targets[0].rc == 0 && targets[1].rc == 0 && targets[2].rc == -1 &&
          !targets[2].attr.cmd_open && targets[3].rc == 0 && targets[4].rc == 0 &&
          visits[2] == 1 && tick_failures == 1, "short byte count rejects partial row and resumes later rows");
    fixture(major, 5); fault_row = 2; batch();
    CHECK(targets[1].rc == 0 && targets[2].rc == -1 && targets[3].rc == 0,
          "faulted row is final while neighbours survive");
    fixture(major, 5); reader_gone = 1; batch();
    CHECK(calls == 1 && !opens && !openats && !reads && !closes && tick_failures == 5,
          "reader ESRCH is bounded without own-pid fallback");
    for (size_t i = 0; i < population; i++)
        CHECK(targets[i].rc == -1 && !targets[i].attr.cmd_open,
              "untransferred rows cannot be parsed as valid");
    fixture(major, 5); put(2, &layout.st_procpid, 9999); batch();
    CHECK(targets[2].rc == -1 && !cache[2].status_addr && !cache[2].shared,
          "slot reuse rejects PID and drops cached address");
    put(2, &layout.st_procpid, 1002);
    CHECK(pgwt_pgbs_reader_resolve(&cache[2], 42, (uintptr_t)&entries[2], &layout) == 0,
          "slot reuse re-resolves through own pid");
    batch(); CHECK(targets[2].rc == 0, "re-resolved slot is usable");
    fixture(major, 5); targets[1].reader = &cache[2]; targets[2].reader = &cache[1]; batch();
    CHECK(targets[1].rc == -1 && targets[2].rc == -1 && targets[0].rc == 0,
          "cross-wired cache enrollments fail closed");
    fixture(major, 5); cache[1].status_addr = entries[2]; cache[2].status_addr = entries[1]; batch();
    CHECK(targets[1].rc == -1 && targets[2].rc == -1,
          "cross-wired row buffers fail PID identity");
    fixture(major, 5); put(2, &layout.st_state, major == 18 ? 2 : 1); batch();
    CHECK(targets[2].rc == 0 && !targets[2].attr.cmd_open, "idle row is observed");
    put(2, &layout.st_state, layout.state_running);
    strcpy(activity[2], "SELECT 'next'");
    if (major != 13) put(2, &layout.st_query_id, 9876);
    batch();
    CHECK(targets[2].rc == 0 && targets[2].attr.cmd_open && visits[2] == 4 &&
          (major == 13 ? strcmp(targets[2].attr.activity, "SELECT 'next'") == 0 :
           targets[2].attr.query_id == 9876),
          "idle row changes on the next tick");
    fixture(major, 5); mutate_on_span = 1; batch();
    CHECK(targets[2].rc == -1 && calls == 3, "counter after payload detects concurrent writer");
    fixture(major, 5); cache[2].shared = false; batch();
    CHECK(targets[2].rc == -1 && visits[2] == 0 && targets[1].rc == 0,
          "unproven shared address is never batched");
    fixture(major, 5); put(2, &layout.st_state, layout.state_max + 1); batch();
    CHECK(targets[2].rc == -1, "invalid state fails closed");
    if (major != 13) {
        fixture(major, 5); put(2, &layout.st_databaseid, 0); batch();
        CHECK(targets[2].rc == PGWT_PGBS_ATTR_NO_CONTEXT, "NO_CONTEXT stays distinct");
    }
    /* Partial transfers on both sides of the 1024-iovec chunk boundary. */
    fixture(major, 1024); short_row = major == 13 ? 256 : 341; batch();
    CHECK(targets[short_row-1].rc == 0 && targets[short_row].rc == -1 &&
          targets[short_row+1].rc == 0 && targets[1023].rc == 0,
          "short row at chunk boundary never borrows previous bytes");
    fixture(major, 1024);
    for (size_t i = 0; i < population; i++) put(i, &layout.st_changecount, 21);
    batch();
    CHECK(calls == (major == 13 ? 6 : 5) && tick_failures == 1024 && visits[1023] == 2,
          "large torn set has at most two retry calls and one failure per backend");
    printf("PG%d fault/identity/idle/chunk cases reached\n", major);
}
static void test_resolve(int major)
{
    fixture(major, 3);
    struct pgwt_pgbs_reader r;
    CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0 && r.start_time == 1 && !opens,
          "enrollment stores start time without descriptor");
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == 0 &&
          r.shared && r.status_addr == entries[1] && opens == closes,
          "own-pid resolution caches coherent shared row and closes mem");
    private_target = 1;
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == -1 && !r.shared,
          "private target mapping refuses proof");
    private_target = 0; private_reader = 1;
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == -1 && !r.shared,
          "private postmaster mapping refuses proof");
    private_reader = 0; incarnation = 2; reset_counts();
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == -1 && !opens,
          "own-pid resolution refuses replacement incarnation");
    pgwt_pgbs_reader_reset(&r); CHECK(!r.pid && !r.status_addr, "retirement clears row proof");
    fixture(major, 3); reset_counts();
    pgwt_pgbs_read_batch(42, &layout, targets, population, true, &tick_failures);
    CHECK(calls == 1 && targets[1].rc == 0 && targets[1].gate.cmd_open &&
          targets[1].gate.query_id_valid == (major != 13) && !opens && !reads,
          "command recheck uses one foreign-pid batch and same predicate");
}
int main(void)
{
    (void)io_openat; /* catches an accidentally reintroduced openat */
    for (int i = 0; i < 3; i++) {
        const int major[] = {13,17,18};
        test_budget(major[i], 512, 2);
        test_budget(major[i], 1024, major[i] == 13 ? 4 : 3);
        test_faults(major[i]); test_resolve(major[i]);
    }
    printf("sampled_attr_io: %d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}
