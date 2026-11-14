/* Compile-time I/O injection counts the actual libc calls in the production
 * translation unit, including any open accidentally added outside a helper.
 * No /proc, BPF, PostgreSQL or timing assumptions: runnable on macOS. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pg_wait_tracer.h"

static FILE *io_fopen(const char *, const char *);
static int io_open(const char *, int, ...);
static int io_openat(int, const char *, int, ...);
static int io_close(int);
static ssize_t io_pread(int, void *, size_t, off_t);
#define fopen io_fopen
#define open io_open
#define openat io_openat
#define close io_close
#define pread io_pread
#ifndef PGWT_PGBS_SOURCE
#define PGWT_PGBS_SOURCE "../src/backend_status_layout.c"
#endif
#include PGWT_PGBS_SOURCE
#undef fopen
#undef open
#undef openat
#undef close
#undef pread

static int checks, failures, opens, openats, closes, reads;
static int incarnation = 1, pinned, directory_incarnation, live = 1;
static int fail_open, replace_during_open, torn, odd, short_row, unread_activity;
static uint8_t row[440];
static uint64_t entry;
static char activity[64] = "SELECT 42";
static struct PgBackendStatusLayout layout;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL: %s (open=%d openat=%d pread=%d close=%d)\n", \
            msg, opens, openats, reads, closes); } } while (0)

/* Only enrollment/retry uses stat. Keep the real parser, injecting its
 * /proc file contents so starttime/PID reuse are exercised on macOS too. */
static FILE *io_fopen(const char *path, const char *mode)
{
    if (!strstr(path, "/stat")) return fopen(path, mode);
    if (!live) { errno = ESRCH; return NULL; }
    FILE *fp = tmpfile();
    if (!fp) abort();
    fputs("42 (postgres) S", fp);
    for (int field = 4; field < 22; field++) fputs(" 0", fp);
    fprintf(fp, " %d\n", incarnation);
    rewind(fp);
    return fp;
}
static int io_open(const char *path, int flags, ...)
{
    (void)flags;
    opens++;
    if (fail_open) { errno = EACCES; return -1; }
    if (strstr(path, "/mem")) {
        pinned = incarnation;
        return 101;
    }
    directory_incarnation = incarnation;
    if (replace_during_open) { incarnation++; live = 1; }
    return 100;
}
static int io_openat(int fd, const char *path, int flags, ...)
{
    (void)path; (void)flags;
    openats++;
    if (fd != 100 || directory_incarnation != incarnation || !live) {
        errno = ESRCH;
        return -1;
    }
    pinned = directory_incarnation;
    return 101;
}
static int io_close(int fd)
{
    CHECK(fd == 100 || fd == 101, "close owns only its descriptor");
    closes++;
    return 0;
}
static ssize_t io_pread(int fd, void *buf, size_t size, off_t offset)
{
    reads++;
    if (fd != 101 || !live || pinned != incarnation) return 0;
    uintptr_t addr = (uintptr_t)offset;
    if (addr >= (uintptr_t)row && addr < (uintptr_t)row + sizeof(row) &&
        size > 8 && short_row)
        return (ssize_t)size - 1;
    if (addr == (uintptr_t)activity && unread_activity) return -1;
    /* Fault immediately beyond the required field span, even when unused
     * struct padding is larger. The production bulk read must not need it. */
    uint32_t end = layout.major == 13
        ? layout.st_activity_raw.offset + layout.st_activity_raw.width
        : layout.st_query_id.offset + layout.st_query_id.width;
    if (addr >= (uintptr_t)row && addr < (uintptr_t)row + sizeof(row) &&
        addr + size > (uintptr_t)row + end) { errno = EFAULT; return -1; }
    if (addr == (uintptr_t)row + layout.st_changecount.offset && size == 4) {
        uint32_t count;
        memcpy(&count, row + layout.st_changecount.offset, 4);
        if (torn) { count += 2; memcpy(row + layout.st_changecount.offset, &count, 4); }
        if (odd) count |= 1u;
        memcpy(buf, &count, 4);
        return 4;
    }
    if (!((addr >= (uintptr_t)row && addr + size <= (uintptr_t)row + sizeof(row)) ||
          (addr == (uintptr_t)&entry && size == sizeof(entry)) ||
          (addr == (uintptr_t)activity && size <= sizeof(activity)))) {
        errno = EFAULT;
        return -1;
    }
    memcpy(buf, (const void *)addr, size);
    return (ssize_t)size;
}
static void reset_counts(void) { opens = openats = closes = reads = 0; }
static void put(const struct pgwt_pgbs_field *field, uint64_t value)
{
    memcpy(row + field->offset, &value, field->width);
}
static void fixture(int major)
{
    CHECK(pgwt_pgbs_hard_table_lookup(major, PGWT_PGBS_ARCH_X86_64, 8,
          PGWT_PGBS_ABI_SYSV_LP64, &layout) == 0, "fixture layout exists");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    struct pgwt_pgbs_field *fields[] = {&layout.st_changecount,
        &layout.st_procpid, &layout.st_databaseid, &layout.st_userid,
        &layout.st_state, &layout.st_activity_raw, &layout.st_query_id};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (fields[i]->present) fields[i]->validation = PGWT_PGBS_FIELD_VALIDATED;
    memset(row, 0, sizeof(row));
    layout.activity_buffer_size = sizeof(activity);
    layout.status_anchor = entry = (uintptr_t)row;
    put(&layout.st_changecount, 20);
    put(&layout.st_procpid, 42);
    put(&layout.st_databaseid, 16384);
    put(&layout.st_userid, 10);
    put(&layout.st_state, layout.state_running);
    put(&layout.st_activity_raw, (uintptr_t)activity);
    if (major != 13) put(&layout.st_query_id, 0xf123456789abcdefULL);
    live = 1;
    torn = odd = short_row = unread_activity = fail_open = replace_during_open = 0;
    reset_counts();
}
static int attr(struct pgwt_pgbs_reader *reader, struct pgwt_pgbs_sampled_attr *out)
{
    return pgwt_pgbs_reader_attr(reader, 42, (uintptr_t)&entry,
                                layout.major == 13 ? entry : 0, &layout, out);
}
static void test_version(int major)
{
    fixture(major);
    struct pgwt_pgbs_sampled_attr out;
    int rc = major == 13
        ? pgwt_pgbs_read_sampled_attr_at(42, entry, &layout, &out)
        : pgwt_pgbs_read_sampled_attr(42, (uintptr_t)&entry, &layout, &out);
    CHECK(rc == 0 && out.cmd_open, "baseline reaches command case");
    CHECK(opens == 1 && openats == 0 && reads == 8 && closes == 1,
          "baseline attribution is ten syscalls");
    printf("PG%d baseline: open=1 pread=8 close=1 total=10\n", major);

    struct pgwt_pgbs_reader reader = {0};
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == 0, "enrollment opens pinned reader");
    CHECK(attr(&reader, &out) == 0 && out.cmd_open && out.databaseid == 16384 &&
          out.userid == 10, "first cached read preserves command/context");
    CHECK(reader.status_addr == entry, "coherent identity caches row");
    uint64_t saved_entry = entry;
    if (major != 13) {
        reader.status_addr = 0; entry = 0;
        CHECK(attr(&reader, &out) == -1 && !reader.status_addr && !reader.dead,
              "early null MyBEEntry remains retryable on pinned mm");
        entry = saved_entry;
        CHECK(attr(&reader, &out) == 0 && reader.status_addr == entry,
              "initialized MyBEEntry is discovered without reopening");
    }
    reset_counts();
    for (int i = 0; i < 500; i++) CHECK(attr(&reader, &out) == 0, "steady tick succeeds");
    int per_tick = major == 13 ? 4 : 3;
    CHECK(opens == 0 && openats == 0 && closes == 0 && reads == 500 * per_tick,
          "steady attribution syscall budget and no opens");
    printf("PG%d cached: open=0 pread=%d close=0 total=%d per backend/tick (500 ticks)\n",
           major, per_tick, per_tick);
    if (major == 13) CHECK(strcmp(out.activity, activity) == 0, "PG13 activity preserved");
    else CHECK(out.query_id == 0xf123456789abcdefULL, "all query id bits preserved");

    put(&layout.st_state, major == 18 ? 2 : 1); /* PostgreSQL STATE_IDLE */
    reset_counts();
    CHECK(attr(&reader, &out) == 0 && !out.cmd_open && !out.query_id && !out.activity[0],
          "fresh idle state closes gate");
    CHECK(reads == per_tick, "idle backend receives full fresh snapshot");
    put(&layout.st_state, layout.state_running);
    CHECK(attr(&reader, &out) == 0 && out.cmd_open, "idle-to-active never skipped");
    struct pgwt_pgbs_cmd_gate gate;
    reset_counts();
    CHECK(pgwt_pgbs_reader_cmd_gate(&reader, 42, (uintptr_t)&entry, entry,
          &layout, &gate) == 0 && gate.cmd_open, "fresh command gate succeeds");
    CHECK(reads == 3 && !opens && !openats && !closes, "fresh gate has no open/close");

    torn = 1; reset_counts();
    CHECK(attr(&reader, &out) == -1 && !out.cmd_open, "torn snapshot fails closed");
    CHECK(reads == 32 * per_tick, "all 32 torn-read attempts remain bounded");
    torn = 0; odd = 1; reset_counts();
    CHECK(attr(&reader, &out) == -1 && reads == 32, "odd counter refuses before fields");
    odd = 0; short_row = 1;
    CHECK(attr(&reader, &out) == -1 && !out.query_id, "short bulk copy fails closed");
    short_row = 0;
    put(&layout.st_procpid, 99);
    CHECK(attr(&reader, &out) == -1 && !out.cmd_open, "wrong row PID refuses");
    CHECK(reader.status_addr == 0, "identity mismatch invalidates row cache");
    put(&layout.st_procpid, 42);
    if (major != 13) {
        put(&layout.st_databaseid, 0);
        CHECK(attr(&reader, &out) == PGWT_PGBS_ATTR_NO_CONTEXT && !out.cmd_open,
              "context-less coherent row retains distinct positive outcome");
        put(&layout.st_databaseid, 16384);
    }
    if (major == 13) {
        unread_activity = 1;
        CHECK(attr(&reader, &out) == 0 && out.cmd_open && !out.activity[0],
              "unreadable activity keeps coherent command unattributed");
        unread_activity = 0;
    }
    live = 0; reset_counts();
    CHECK(attr(&reader, &out) == -1, "exit between ticks refuses old mm");
    incarnation++; live = 1; /* same numeric PID/address/content, new process */
    CHECK(attr(&reader, &out) == -1 && !out.cmd_open, "PID reuse never redirects cached mm");
    CHECK(!opens && !openats && !closes, "dead reader is never reopened on tick");
    pgwt_pgbs_reader_close(&reader);
    reset_counts(); pgwt_pgbs_reader_close(&reader);
    CHECK(!closes, "retirement close is idempotent");
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == 0 && attr(&reader, &out) == 0,
          "fresh enrollment can bind replacement PID");
    pgwt_pgbs_reader_close(&reader);

    fixture(major); reader = (struct pgwt_pgbs_reader){0};
    replace_during_open = 1;
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == -1,
          "proc directory prevents PID replacement during enrollment");
    replace_during_open = 0; reset_counts();
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == -1 && !opens && !openats,
          "failed enrollment cannot retry against replacement");
    CHECK(attr(&reader, &out) == -1 && !reads, "failed enrollment refuses samples");
    pgwt_pgbs_reader_close(&reader);

    fixture(major); reader = (struct pgwt_pgbs_reader){0};
    fail_open = 1;
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == -1 && reader.open_error == EACCES,
          "open failure retains errno and original identity");
    fail_open = 0;
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == 0 && attr(&reader, &out) == 0,
          "transient open failure retries original incarnation");
    reset_counts();
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == 0 && !opens && !openats,
          "same enrollment never opens a second descriptor");
    CHECK(pgwt_pgbs_reader_attr(&reader, 99, (uintptr_t)&entry, entry,
          &layout, &out) == -1 && !reads, "foreign enrollment PID refuses before I/O");
    pgwt_pgbs_reader_close(&reader);

    fixture(major); reader = (struct pgwt_pgbs_reader){0};
    fail_open = 1;
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == -1, "failed enrollment retains identity");
    fail_open = 0; incarnation++; reset_counts();
    CHECK(pgwt_pgbs_reader_open(&reader, 42) == -1 && !opens && !openats,
          "retry cannot open a replacement PID");
    pgwt_pgbs_reader_close(&reader);
}
static void test_churn(void)
{
    fixture(18);
    struct pgwt_pgbs_reader reader = {0};
    for (int i = 0; i < MAX_BACKENDS + 1; i++) {
        CHECK(pgwt_pgbs_reader_open(&reader, 42) == 0, "churn enrolls");
        pgwt_pgbs_reader_close(&reader);
    }
    CHECK(opens + openats == closes, "churn closes every enrollment descriptor");
    printf("descriptor churn: %d enrollments, %d opens, %d closes\n",
           MAX_BACKENDS + 1, opens + openats, closes);
}
int main(void)
{
    test_version(18); test_version(17); test_version(13); test_churn();
    printf("sampled_attr_io: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
