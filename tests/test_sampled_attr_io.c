/* Inject libc and process_vm_readv into the production translation unit.
 * Runs on macOS: no /proc, PostgreSQL, BPF, timing, or kernel assumptions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
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
static ssize_t io_read(int, void *, size_t);
static ssize_t io_readv(pid_t, const struct iovec *, unsigned long,
                        const struct iovec *, unsigned long, unsigned long);
#define fopen io_fopen
#define open io_open
#define openat io_openat
#define close io_close
#define pread io_pread
#define read io_read
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
#undef read
#undef process_vm_readv

static int checks, failures, opens, openats, closes, reads, calls, fopens;
static int own_calls, stat_reads, map_reads, stat_fail, own_short;
static int proc_opens, proc_reads, proc_closes, reader_map_reads, maps_fail, wait_calls;
static int maps_prefix_lines;
static uint32_t wait_words[1024];
static uint64_t refusal_wait_addr;
static uint32_t refusal_wait_value;
static FILE *proc_file;
static int private_target, private_reader, incarnation = 1;
static int tear_row = -1, transient_tear, short_row = -1, fault_row = -1;
static int reader_gone, mutate_on_span, max_iovs, after_copy[1024], visits[1024];
/* Realistic shared surroundings: scanning cannot infer the array bounds from
 * /proc/maps. PGPROC slot order deliberately differs from backendId order. */
#define STORAGE_BYTES (1024 * 440)
static struct {
    uint8_t below[600 * 440];
    uint8_t storage[STORAGE_BYTES];
    uint8_t above[600 * 440];
    uint64_t entries[1024];
    char activity[1024][64];
    uint8_t pgproc[1024][856];
} arena;
static unsigned proc_slot(size_t i) { return (unsigned)(i * 641 + 317) % 1024; }
static uint64_t proc_at(size_t i) { return (uintptr_t)arena.pgproc[proc_slot(i)]; }
#ifdef PGWT_LIFECYCLE_SOURCE
static uint64_t test_wait(int major, size_t i)
{ return major == 13 ? proc_at(i) + 684 : i == 0 ? 1234 : 2345; }
static void proc_identity(size_t i, uint32_t pid, uint32_t id)
{
    memcpy((void *)(uintptr_t)(proc_at(i) + 52), &pid, 4);
    memcpy((void *)(uintptr_t)(proc_at(i) + 60), &id, 4);
}
#endif
static unsigned mapping_incarnation = 1;
static int exclude_rows;
static int32_t anchor_backend_id;
static uint8_t *const storage = arena.storage;
static uint64_t *const entries = arena.entries;
static char (*const activity)[64] = arena.activity;
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
        stat_reads++;
        if (stat_fail) { fclose(fp); errno = ENOENT; return NULL; }
        fputs("1000 (postgres) S 42", fp);
        for (int field = 5; field < 22; field++) fputs(" 0", fp);
        fprintf(fp, " %d\n", incarnation);
    } else {
        map_reads++;
        if (strstr(path, "/42/maps")) {
            reader_map_reads++;
            if (maps_fail) { fclose(fp); errno = EACCES; return NULL; }
        }
        int pid = 0;
        (void)sscanf(path, "/proc/%d/maps", &pid);
        const char *perms = (pid == 42 ? private_reader : private_target) ? "rw-p" : "rw-s";
        for (int i = 0; i < maps_prefix_lines; i++)
            fputs("00001000-00002000 rw-p 00000000 00:00 0 /padding\n", fp);
        if (exclude_rows && (exclude_rows == 1 ? pid != 42 : pid == 42)) {
            fprintf(fp, "%llx-%llx %s 00000000 00:00 3\n",
                (unsigned long long)(uintptr_t)arena.pgproc,
                (unsigned long long)(uintptr_t)(arena.pgproc + 1024), perms);
        } else fprintf(fp, "%llx-%llx %s 00000000 00:00 %u\n",
            (unsigned long long)(uintptr_t)&arena,
            (unsigned long long)(uintptr_t)(&arena + 1), perms, mapping_incarnation);
        fprintf(fp, "%llx-%llx %s 00000000 00:00 2\n",
            (unsigned long long)(uintptr_t)activity,
            (unsigned long long)(uintptr_t)(activity + population), perms);
    }
    rewind(fp);
    return fp;
}
static int io_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strstr(path, "/stat") || strstr(path, "/maps")) {
        proc_opens++; proc_file = io_fopen(path, "r"); fopens--;
        return proc_file ? 103 : -1;
    }
    opens++; return 101;
}
static int io_openat(int fd, const char *path, int flags, ...)
{ (void)fd; (void)path; (void)flags; openats++; return 101; }
static int io_close(int fd) {
    if (fd == 103) { proc_closes++; fclose(proc_file); proc_file = NULL; return 0; }
    closes++; return 0;
}
static ssize_t io_read(int fd, void *buf, size_t size)
{
    if (fd != 103) { errno = EBADF; return -1; }
    proc_reads++; return (ssize_t)fread(buf, 1, size, proc_file);
}
static ssize_t io_pread(int fd, void *buf, size_t size, off_t offset)
{
    (void)fd; reads++;
    uintptr_t addr = (uintptr_t)offset;
    if (addr == (uintptr_t)&anchor_backend_id && size == 4) {
        memcpy(buf, &anchor_backend_id, size); return (ssize_t)size;
    }
    if (addr >= (uintptr_t)&arena && addr + size <= (uintptr_t)(&arena + 1)) {
        memcpy(buf, (void *)addr, size); return (ssize_t)size;
    }
    errno = EFAULT; return -1;
}
static void put(size_t i, const struct pgwt_pgbs_field *field, uint64_t value)
{ memcpy(row_at(i) + field->offset, &value, field->width); }
static ssize_t io_readv(pid_t pid, const struct iovec *local, unsigned long nl,
                        const struct iovec *remote, unsigned long nr, unsigned long flags)
{
    if (pid != 42) {
        own_calls++;
        CHECK(pid >= 1000 && pid < 2024 && flags == 0 && nl == nr,
              "candidate lookup reads only its own backend");
        if (own_short) return 1;
        if (nr == 1 && (uintptr_t)remote[0].iov_base == UINT64_C(0x1234000)) {
            memcpy(local[0].iov_base, &entries[pid-1000], sizeof(uint64_t));
            return sizeof(uint64_t);
        }
    } else calls++;
    CHECK(flags == 0 && nl == nr, "matching VM range counts");
    if ((int)nr > max_iovs) max_iovs = (int)nr;
    if (nr > 1024) { errno = EINVAL; return -1; }
    if (pid == 42 && reader_gone) { errno = ESRCH; return -1; }
    size_t total = 0;
    for (size_t j = 0; j < nr; j++) {
        uintptr_t addr = (uintptr_t)remote[j].iov_base;
        size_t len = remote[j].iov_len;
        CHECK(len == local[j].iov_len, "matching local and remote ranges");
        if (addr >= (uintptr_t)storage && addr < (uintptr_t)(storage + STORAGE_BYTES)) {
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
            if (pid == 42 && ((int)i == fault_row || ((int)i == short_row && len > 4))) {
                if (total) return (ssize_t)(total + ((int)i == short_row ? len - 1 : 0));
                errno = EFAULT; return -1;
            }
            if (pid == 42 && offset == layout.st_changecount.offset && len == 4) {
                visits[i]++;
                uint32_t value;
                memcpy(&value, (void *)addr, 4);
                if ((int)i == tear_row && (!transient_tear || after_copy[i] < 2)) {
                    value += 2;
                    memcpy((void *)addr, &value, 4);
                    after_copy[i]++;
                }
            } else if (pid == 42 && len > 4 && mutate_on_span) {
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
ssize_t pgwt_test_wait_readv(pid_t pid, const struct iovec *local, unsigned long nl,
    const struct iovec *remote, unsigned long nr, unsigned long flags)
{
    wait_calls++;
    CHECK(pid == 1000 && nl == nr && flags == 0, "wait batch uses one eligible reader");
    size_t copied = 0;
    for (size_t i = 0; i < nr; i++) {
        if (refusal_wait_addr) {
            CHECK((uintptr_t)remote[i].iov_base == refusal_wait_addr &&
                  remote[i].iov_len == 4,
                  "refusal probe reads the expected incarnation's wait slot");
            memcpy(local[i].iov_base, &refusal_wait_value, 4);
            copied += 4;
            continue;
        }
        CHECK((uintptr_t)remote[i].iov_base == (layout.major == 13
                  ? proc_at(i) + 684 : (uintptr_t)&wait_words[i]) && remote[i].iov_len == 4,
              "wait batch reads successor-independent shared wait slots");
        memcpy(local[i].iov_base, &wait_words[i], remote[i].iov_len);
        copied += remote[i].iov_len;
    }
    return (ssize_t)copied;
}
static void reset_counts(void)
{
    opens = openats = closes = reads = calls = max_iovs = 0;
    tick_failures = 0; fopens = own_calls = stat_reads = map_reads = 0;
    proc_opens = proc_reads = proc_closes = reader_map_reads = wait_calls = 0;
    memset(visits, 0, sizeof(visits)); memset(after_copy, 0, sizeof(after_copy));
}
static void fixture(int major, size_t n)
{
    for (size_t i = 0; i < 1024; i++) pgwt_pgbs_reader_reset(&cache[i]);
    private_target = private_reader = reader_gone = mutate_on_span = transient_tear = 0;
    incarnation = 1; stat_fail = own_short = maps_fail = maps_prefix_lines = 0;
    population = n;
    CHECK(pgwt_pgbs_hard_table_lookup(major, PGWT_PGBS_ARCH_X86_64, 8,
          PGWT_PGBS_ABI_SYSV_LP64, &layout) == 0, "fixture layout exists");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    struct pgwt_pgbs_field *fields[] = {&layout.st_changecount, &layout.st_procpid,
        &layout.st_databaseid, &layout.st_userid, &layout.st_state,
        &layout.st_activity_raw, &layout.st_query_id};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (fields[i]->present) fields[i]->validation = PGWT_PGBS_FIELD_VALIDATED;
    memset(storage, 0, STORAGE_BYTES); memset(targets, 0, sizeof(targets));
    layout.activity_buffer_size = sizeof(activity[0]);
    layout.status_anchor = (uintptr_t)row_at(511);
    layout.status_array_base = (uintptr_t)storage;
    layout.pgproc_pid_offset = 52;
    layout.pgproc_backend_id_offset = 60;
    mapping_incarnation = 1; exclude_rows = 0;
    layout.shm_generation = pg13_mapping_generation(42, layout.status_anchor);
    layout.generation_postmaster = 42;
    layout.pg13_computed = major == 13;
    memset(arena.below, 0, sizeof(arena.below));
    for (size_t i = 0; i < sizeof(arena.below) / layout.struct_size; i++) {
        uint32_t decoy = 1000 + i % (n ? n : 1);
        uintptr_t base = (uintptr_t)storage - (i + 1) * layout.struct_size;
        uint64_t pointer = (uintptr_t)activity[decoy - 1000];
        memcpy((void *)(base + layout.st_procpid.offset), &decoy, sizeof(decoy));
        memcpy((void *)(base + layout.st_activity_raw.offset), &pointer, sizeof(pointer));
    }
    memset(arena.pgproc, 0, sizeof(arena.pgproc));
    for (size_t i = 0; i < 1024; i++) {
        uint32_t pid = 1000 + i, slot = proc_slot(i), id = i + 1;
        memcpy((void *)(uintptr_t)(proc_at(i) + 52), &pid, 4);
        memcpy((void *)(uintptr_t)(proc_at(i) + 56), &slot, 4);
        memcpy((void *)(uintptr_t)(proc_at(i) + 60), &id, 4);
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
            .shared_reader_pid = 42, .shared = true,
            .shm_generation = layout.shm_generation};
        targets[i].pid = (pid_t)(1000 + i); targets[i].reader = &cache[i];
        targets[i].rc = -1;
        targets[i].pgproc_addr = proc_at(i);
    }
    private_target = private_reader = reader_gone = mutate_on_span = transient_tear = 0;
    incarnation = 1; stat_fail = own_short = maps_fail = maps_prefix_lines = 0;
    refusal_wait_addr = 0;
    tear_row = short_row = fault_row = -1;
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
        if (failures) break;
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
    if (major == 13) {
        struct pgwt_pgbs_maps_cache maps = {0};
        pgwt_pgbs_resolve_pg13_batch(42, &layout, targets, population, &maps, NULL);
        pgwt_pgbs_maps_cache_reset(&maps);
    } else {
        CHECK(pgwt_pgbs_reader_resolve(&cache[2], 42, (uintptr_t)&entries[2], &layout, NULL) == 0,
              "slot reuse re-resolves through own pid");
    }
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
    struct pgwt_pgbs_reader r = {0};
    CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0 && r.start_time == 1 && stat_reads == 1,
          "enrollment stores start time without descriptor");
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout, NULL) == 0 &&
          r.shared && r.status_addr == entries[1] && own_calls == 1 &&
          !opens && !closes && !reads && stat_reads == 1 && map_reads == 2,
          "one own-pid VM call caches candidate without re-reading stat");
    /* No pre-batch identity acceptance: even an alien row can be a hint. */
    targets[1].reader = &r;
    put(1, &layout.st_procpid, 9999); batch();
    CHECK(targets[1].rc == -1 && !r.status_addr,
          "only the shared batch accepts candidate PID identity");
    put(1, &layout.st_procpid, 1001);
    reset_counts();
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout, NULL) == 0 &&
          own_calls == 1 && !stat_reads && !map_reads,
          "invalidated candidate reuses mapping snapshots");
    reset_counts();
    for (int tick = 0; tick < 500; tick++)
        CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout, NULL) == 0,
              "cached row resolver succeeds");
    CHECK(!own_calls && !map_reads && !stat_reads,
          "cached-address return prevents re-resolution on every tick");
    batch(); CHECK(targets[1].rc == 0, "re-resolved candidate is accepted by batch");
    pgwt_pgbs_reader_reset(&r);
    for (int reader_private = 0; reader_private < 2; reader_private++) {
        CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0, "mapping fixture enrolls");
        private_target = !reader_private; private_reader = reader_private;
        CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout, NULL) == -1 &&
              !r.shared && !r.status_addr, "both mappings must prove shared");
        pgwt_pgbs_reader_reset(&r);
    }
    private_target = private_reader = 0;
    CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0, "short-pointer fixture enrolls");
    own_short = 1;
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout, NULL) == -1 &&
          !r.status_addr, "short own-pid read never supplies a candidate");
    own_short = 0;
    pgwt_pgbs_reader_reset(&r); CHECK(!r.pid && !r.target_maps, "retirement clears row and mapping proof");
    fixture(major, 3); reset_counts();
    pgwt_pgbs_read_batch(42, &layout, targets, population, true, &tick_failures);
    CHECK(calls == 1 && targets[1].rc == 0 && targets[1].gate.cmd_open &&
          targets[1].gate.query_id_valid == (major != 13) && !opens && !reads,
          "command recheck uses one foreign-pid batch and same predicate");
}

static void pg13_anchor(size_t slot)
{
    anchor_backend_id = (int32_t)slot + 1;
    layout.my_backend_id_addr = (uintptr_t)&anchor_backend_id;
    pg13_record_anchor(&layout, 1000 + (pid_t)slot, 42, (uintptr_t)row_at(slot));
    CHECK(layout.status_array_base == (uintptr_t)storage && layout.pg13_computed,
          "PG13 anchor ID establishes array base independent of anchor position");
}
static uint64_t scan_uses;
static void pg13_discover(size_t n)
{
    struct pgwt_pgbs_maps_cache maps = {0};
    pgwt_pgbs_resolve_pg13_batch(42, &layout, targets, n, &maps, &scan_uses);
    pgwt_pgbs_maps_cache_reset(&maps);
}
static void pg13_pending(size_t i)
{
    pgwt_pgbs_reader_reset(&cache[i]);
    CHECK(pgwt_pgbs_reader_enroll(&cache[i], 1000 + (pid_t)i) == 0,
          "PG13 pending enrollment");
}
static void test_pg13_computed(void)
{
    const size_t counts[] = {500, 1000, 1024};
    const size_t anchors[] = {0, 511, 1023};
    for (size_t c = 0; c < 3; c++) for (size_t a = 0; a < 3; a++) {
        size_t n = counts[c];
        fixture(13, n); pg13_anchor(anchors[a]);
        for (size_t i = 0; i < n; i++) pg13_pending(i);
        reset_counts(); scan_uses = 0;
        pg13_discover(n);
        CHECK(calls == 2 && !own_calls && max_iovs <= 1024 && !scan_uses,
              "PG13 pending discovery uses at most two bounded postmaster calls");
        batch();
        for (size_t i = 0; i < n; i++)
            CHECK(targets[i].rc == 0 && cache[i].status_addr == entries[i] &&
                  targets[i].attr.databaseid == 16384 && targets[i].attr.userid == 10 &&
                  strcmp(targets[i].attr.activity, activity[i]) == 0,
                  "PG13 all eligible rows are attributed on their first tick");
        CHECK(!tick_failures, "PG13 initialized first tick has no dropped samples");
        reset_counts(); pg13_discover(n);
        CHECK(!calls && !own_calls && !stat_reads && !map_reads && !scan_uses,
              "PG13 steady state performs zero discovery calls");
        printf("PG13 computed N=%zu anchor=%zu: first eligible tick, discovery=2, steady=0, iov<=1024\n",
               n, anchors[a]);
    }
    const int distances[] = {-1023, -513, -257, 257, 513, 1023};
    for (size_t d = 0; d < sizeof(distances)/sizeof(*distances); d++) {
        fixture(13, 1024);
        int distance = distances[d];
        size_t anchor = distance < 0 ? 1023 : 0;
        size_t i = (size_t)((int)anchor + distance);
        pg13_anchor(anchor); pg13_pending(i);
        reset_counts(); scan_uses = 0; pg13_discover(1024);
        CHECK(calls == 2 && max_iovs == 1 && !own_calls && !scan_uses,
              "PG13 one pending far row uses two single-range calls");
        batch();
        CHECK(targets[i].rc == 0 && cache[i].status_addr == entries[i],
              "PG13 signed far distances resolve on the first eligible tick");
    }
    /* The backendId is assigned before pgstat_bestart initializes its row.
     * A rejected hint must retry on the very next tick, without backoff. */
    fixture(13, 1024); pg13_anchor(0); pg13_pending(1023);
    put(1023, &layout.st_procpid, 0); reset_counts(); pg13_discover(1024); batch();
    CHECK(targets[1023].rc < 0 && tick_failures == 1,
          "PG13 preinitialization sample stays counted as unreadable");
    put(1023, &layout.st_procpid, 2023); reset_counts(); pg13_discover(1024); batch();
    CHECK(targets[1023].rc == 0 && calls == 6 && !tick_failures,
          "PG13 initialization after rejection is eligible on the next tick");
}
static void test_pg13_pending_backend_id_known_base(void)
{
    fixture(13, 1); pg13_anchor(511); pg13_pending(0);
    int32_t id = -1;
    memcpy((void *)(uintptr_t)(proc_at(0) + layout.pgproc_backend_id_offset), &id, sizeof(id));
    reset_counts(); scan_uses = 0; pg13_discover(1); batch();
    CHECK(targets[0].rc < 0 && tick_failures == 1 && !cache[0].status_addr &&
          !scan_uses && layout.pg13_computed && calls == 1 && !own_calls,
          "PG13 known base keeps backendId -1 pending without a scan fallback");
    id = 1;
    memcpy((void *)(uintptr_t)(proc_at(0) + layout.pgproc_backend_id_offset), &id, sizeof(id));
    reset_counts(); pg13_discover(1); batch();
    CHECK(targets[0].rc == 0 && cache[0].status_addr == entries[0] &&
          !tick_failures && !scan_uses && layout.pg13_computed && calls == 3 && !own_calls,
          "PG13 initialised backendId resumes the computed route on the next tick");
    puts("test_pg13_pending_backend_id_known_base: PASS (pending counted; computed route; next-tick retry)");
}

static void test_pg13_rejected_computed_hint(void)
{
    fixture(13, 1); pg13_anchor(511); pg13_pending(0);
    put(0, &layout.st_procpid, 0);
    scan_uses = 0;
    for (int tick = 0; tick < 2; tick++) {
        reset_counts(); pg13_discover(1); batch();
        CHECK(targets[0].rc < 0 && tick_failures == 1 &&
              !scan_uses && layout.pg13_computed && calls == 3 && !own_calls,
              "PG13 rejected computed hint is retried without a scan fallback");
    }
    put(0, &layout.st_procpid, 1000);
    reset_counts(); pg13_discover(1); batch();
    CHECK(targets[0].rc == 0 && !tick_failures && !scan_uses && layout.pg13_computed,
          "PG13 rejected computed hint recovers on the next eligible tick");
    puts("test_pg13_rejected_computed_hint: PASS (rejected hints retry computed; no fallback)");
}

static void test_pg13_guards(void)
{
    for (int reader = 1; reader <= 2; reader++) {
        fixture(13, 1); pg13_anchor(511); pg13_pending(0);
        exclude_rows = reader; reset_counts(); pg13_discover(1); batch();
        CHECK(!cache[0].status_addr && targets[0].rc < 0,
              "PG13 computed row requires both shared range proofs");
    }
    fixture(13, 1); pg13_anchor(511); pg13_pending(0);
    int32_t wrong_pid = 9999;
    memcpy((void *)(uintptr_t)(proc_at(0) + 52), &wrong_pid, 4);
    reset_counts(); pg13_discover(1);
    CHECK(!cache[0].computed_hint,
          "PG13 PGPROC pid mismatch is never a computed hint");
    fixture(13, 1); pg13_anchor(511); pg13_pending(0);
    pg13_discover(1); put(0, &layout.st_procpid, 9999); batch();
    CHECK(targets[0].rc < 0, "PG13 computed hints still require the row PID");
    fixture(13, 1); pg13_anchor(511); pg13_pending(0);
    pg13_discover(1); incarnation = 2; batch();
    CHECK(targets[0].rc < 0 && targets[0].error == ESTALE && !cache[0].pid,
          "PG13 computed acceptance checks the enrolled start time");
    fixture(13, 1); put(0, &layout.st_changecount, 21); batch();
    CHECK(targets[0].rc < 0, "PG13 odd counters are refused");
    fixture(13, 1); tear_row = 0; batch();
    CHECK(targets[0].rc < 0, "PG13 unequal counters are refused");
    fixture(13, 3); pg13_anchor(511);
    uint64_t old_generation = layout.shm_generation;
    mapping_incarnation = 2;
    CHECK(pgwt_pgbs_pg13_generation(42, &layout) < 0 &&
          !layout.status_array_base && !layout.status_anchor &&
          layout.validation == PGWT_PGBS_VALIDATION_NOT_RUN,
          "PG13 same-postmaster generation change invalidates layout and base");
    batch(); CHECK(targets[0].rc < 0,
          "PG13 invalidated layout cannot accept cached rows");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED; pg13_anchor(0);
    CHECK(layout.shm_generation != old_generation,
          "PG13 revalidated mapping has a new generation at the same address");
    private_target = 1; reset_counts(); pg13_discover(3); batch();
    CHECK(!cache[0].status_addr && targets[0].rc < 0 && map_reads == 4,
          "PG13 generation change discards accepted rows and mapping proofs");
    fixture(13, 1); pg13_anchor(0);
    CHECK(pgwt_pgbs_pg13_generation(43, &layout) < 0 && !layout.status_array_base,
          "PG13 postmaster replacement invalidates the old layout");
    /* Without a usable anchor ID the retained scan is explicit and budgeted.
     * Its pid-only decoys must never become accepted rows. */
    fixture(13, 1024); layout.status_array_base = 0;
    for (size_t i = 0; i < 1024; i++) pg13_pending(i);
    reset_counts(); scan_uses = 0; pg13_discover(1024); batch();
    CHECK(scan_uses == 1 && !layout.pg13_computed && !own_calls &&
          calls <= 3 && max_iovs <= 1024,
          "PG13 unavailable computed route uses one counted bounded scan");
    for (size_t i = 0; i < population; i++)
        CHECK(targets[i].rc < 0 || cache[i].status_addr == entries[i],
              "PG13 shared pid decoys never pass coherent batch acceptance");
    fixture(13, 1024); pg13_anchor(0); layout.status_array_base = 0;
    for (size_t i = 0; i < 600; i++) {
        uintptr_t base = (uintptr_t)storage - (i + 1) * layout.struct_size;
        uint32_t pid = 2023; uint64_t pointer = (uintptr_t)activity[1023];
        memcpy((void *)(base + layout.st_procpid.offset), &pid, 4);
        memcpy((void *)(base + layout.st_activity_raw.offset), &pointer, 8);
    }
    pg13_pending(1023); targets[0] = targets[1023]; population = 1;
    reset_counts(); scan_uses = 0; pg13_discover(1); batch();
    CHECK(scan_uses == 1 && targets[0].rc < 0 && tick_failures == 1 && calls <= 3,
          "PG13 lower shared pid decoys are rejected by the coherent acceptor");
    puts("PG13 computed identity, mapping, generation and counted fallback guards reached");
}

#ifdef PGWT_LIFECYCLE_SOURCE
/* Only hardware/BPF boundaries are fakes. The Python runner extracts and
 * compiles the production allocation, FORK, enrollment and retry functions;
 * /proc and VM reads still use the same injection layer above. */
#include "daemon.h"
#include "backend_init.h"
#include "idle_rule.h"
#include "discovery.h"
static struct pgwt_sample_target recorded_samples[MAX_BACKENDS];
struct pg_wait_tracer_bpf { struct { void *on_bootstrap; } progs;
                            struct { void *state_map; } maps; };
static uint64_t clock_ns = UINT64_C(10000000000);
static uint64_t now_ns(void) { return clock_ns; }
static uint64_t wait_addr;
static int exit_calls, scan_read;
uint64_t pgwt_debug_monotonic_ns(void) { return clock_ns; }
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t ts)
{ (void)op; (void)pid; (void)ts; }
static int lifecycle_target(struct pgwt_daemon *d, pid_t pid)
{ (void)d; return pid == 1000 ? 1 : -1; }
static int bpf_program__fd(void *p) { (void)p; return 7; }
static int bpf_map__fd(void *p) { (void)p; return 7; }
#define BPF_NOEXIST 1
static int bpf_map_update_elem(int fd, const void *key, const void *val, uint64_t flags)
{ (void)fd; (void)key; (void)val; (void)flags; return 0; }
int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int prog)
{ (void)pid; (void)addr; (void)prog; return 8; }
void pgwt_close_watchpoint(int fd) { (void)fd; }
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *d, pid_t pid)
{ (void)d; (void)pid; return wait_addr; }
int pgwt_addr_is_shared(pid_t pid, uint64_t addr)
{ (void)pid; (void)addr; return 1; }
int pgwt_validate_wait_addr(pid_t pid, uint64_t addr)
{ (void)pid; (void)addr; return PGWT_WEI_VALID_NONZERO; }
int pgwt_parse_cmdline(pid_t pid, struct pgwt_metadata *meta)
{ (void)pid; meta->backend_type = PGWT_BT_CLIENT; return 0; }
void pgwt_bm_write(struct pgwt_backend_meta_writer *bm, pid_t pid, const struct pgwt_metadata *meta)
{ (void)bm; (void)pid; (void)meta; }
const char *pgwt_backend_type_name(enum pgwt_backend_type type) { (void)type; return "client"; }
static void report_state_map_full(struct pgwt_daemon *d, pid_t pid)
{ (void)d; (void)pid; }
static void seed_state_entry(struct pgwt_daemon *d, pid_t pid, uint64_t addr)
{ (void)d; (void)pid; (void)addr; }
static int read_init_state(void *ctx, pid_t pid, struct pgwt_pid_state *state)
{ (void)ctx; (void)pid; memset(state, 0, sizeof(*state)); return 0; }
int pgwt_attach_backend_watchpoint(struct pgwt_daemon *d, struct pgwt_backend *be)
{ (void)d; be->wp_fd = 9; return 0; }
struct pgwt_backend *pgwt_find_backend(struct pgwt_backend_table *bt, pid_t pid)
{
    for (int i = 0; i < bt->count; i++)
        if (bt->entries[i].pid == pid) return &bt->entries[i];
    return NULL;
}
int pgwt_handle_exit(struct pgwt_daemon *d, pid_t pid)
{
    exit_calls++;
    struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
    if (be) { pgwt_pgbs_reader_reset(&be->sampled_reader); be->is_alive = false; be->pid = 0; }
    return 0;
}
static DIR *scan_opendir(const char *path)
{ (void)path; scan_read = 0; return (DIR *)&scan_read; }
static struct dirent *scan_readdir(DIR *dir)
{
    (void)dir;
    static struct dirent ent;
    if (scan_read++) return NULL;
    strcpy(ent.d_name, "1000");
    return &ent;
}
static int scan_closedir(DIR *dir) { (void)dir; return 0; }
#define opendir scan_opendir
#define readdir scan_readdir
#define closedir scan_closedir
#define fopen io_fopen
#define open io_open
#define close io_close
#define pread io_pread
#include PGWT_LIFECYCLE_SOURCE

#undef opendir
#undef readdir
#undef closedir
#undef fopen
#undef open
#undef close
#undef pread

static struct pgwt_daemon *new_daemon(int major)
{
    fixture(major, 1);
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d) abort();
    d->skel = calloc(1, sizeof(*d->skel));
    if (!d->skel) abort();
    d->backend_status_layout = layout;
    d->postmaster_pid = 42;
    d->my_be_entry_addr = (uintptr_t)&entries[0];
    d->mode = PGWT_MODE_TIERED;
    d->pg_major_version = major;
    d->use_myproc = d->wait_offset_validated = major == 13;
    d->pgproc_wait_offset = major == 13 ? 684 : 0;
    wait_addr = 0; exit_calls = 0;
    CHECK(pgwt_sampler_start(d) == 0, "sampler init");
    return d;
}
static void release_daemon(struct pgwt_daemon *d)
{
    for (int i = 0; i < d->backends.count; i++) {
        pgwt_pgbs_reader_reset(&d->backends.entries[i].sampled_reader);
        free(d->backends.entries[i].pg13_synthetic_cache);
    }
    pgwt_sampler_stop(d);
    free(d->skel); free(d);
}
static void test_pg13_registry(void)
{
    struct PgBackendStatusLayout offsets = {0};
    offsets.major = 13; offsets.arch = PGWT_PGBS_ARCH_X86_64;
    offsets.abi = PGWT_PGBS_ABI_SYSV_LP64; offsets.pointer_width = 8;
    pgwt_pg13_status_offsets(&offsets, UINT64_C(0x12345678));
    CHECK(offsets.pgproc_pid_offset == 52 && offsets.pgproc_backend_id_offset == 60 &&
          offsets.my_backend_id_addr == UINT64_C(0x12345678),
          "PG13 discovery wires the probed PGPROC and anchor ID layout");
    offsets.major = 17;
    struct PgBackendStatusLayout unchanged = offsets;
    pgwt_pg13_status_offsets(&offsets, 99);
    CHECK(memcmp(&offsets, &unchanged, sizeof(offsets)) == 0,
          "PG13 status offsets leave later versions unchanged");
    offsets.major = 13; offsets.arch = PGWT_PGBS_ARCH_ARM64;
    unchanged = offsets;
    pgwt_pg13_status_offsets(&offsets, 99);
    CHECK(memcmp(&offsets, &unchanged, sizeof(offsets)) == 0,
          "PG13 status offsets refuse an unprobed architecture");
    const size_t counts[] = {500, 1000, 1024};
    for (size_t c = 0; c < 3; c++) {
        size_t n = counts[c];
        struct pgwt_daemon *d = new_daemon(13);
        fixture(13, n); pg13_anchor(511); d->backend_status_layout = layout;
        for (size_t i = 0; i < n; i++) {
            struct pgwt_backend *be = alloc_backend(&d->backends, 1000 + (pid_t)i, true);
            CHECK(be != NULL, "PG13 registry population fits the existing cap");
            be->wp_addr = proc_at(i) + 684; be->wp_addr_shared = 1; be->meta_parsed = true;
            CHECK(pgwt_sampler_enroll_backend(be, clock_ns) == 0,
                  "PG13 retry-deadline fixture already has an enrolled identity");
            be->pgbs_resolve_retry_ns = clock_ns + UINT64_C(3200000000);
        }
        reset_counts();
        CHECK(sample_registry(d, clock_ns) == (int)n && calls == 2 && !own_calls && max_iovs <= 1024,
              "PG13 sampler batches all pending discovery without a retry deadline");
        batch();
        for (size_t i = 0; i < n; i++) {
            CHECK(targets[i].rc == 0 && d->backends.entries[i].pgbs_addr == entries[i],
                  "PG13 sampler attributes the full population on the first eligible tick");
            finish_sampled_attr(&d->backends.entries[i], targets[i].rc, targets[i].error, clock_ns);
        }
        reset_counts();
        sample_registry(d, clock_ns + 1);
        CHECK(!calls && !own_calls && !stat_reads && map_reads == 1,
              "PG13 sampler steady state only refreshes the generation proof");
        /* The farthest row alone becomes pending and is initially unreadable.
         * finish() may retain a legacy deadline; this route must ignore it. */
        size_t i = n - 1;
        struct pgwt_backend *be = &d->backends.entries[i];
        pgwt_pgbs_reader_reset(&be->sampled_reader);
        put(i, &layout.st_procpid, 0);
        uint64_t t = clock_ns + 2;
        sample_registry(d, t); batch();
        finish_sampled_attr(be, targets[i].rc, targets[i].error, t);
        CHECK(targets[i].rc < 0 && tick_failures == 1,
              "PG13 sampler counts the uninitialised row once");
        put(i, &layout.st_procpid, 1000 + i);
        reset_counts(); sample_registry(d, t + 1);
        CHECK(calls == 2 && max_iovs == 1 && !own_calls,
              "PG13 sampler retries one pending row on the next tick");
        batch(); finish_sampled_attr(be, targets[i].rc, targets[i].error, t + 1);
        CHECK(targets[i].rc == 0 && !tick_failures,
              "PG13 sampler attributes the first initialised tick after rejection");
        release_daemon(d);
    }
    struct pgwt_daemon *d = new_daemon(13);
    struct pgwt_backend *be = alloc_backend(&d->backends, 1000, true);
    be->wp_addr = proc_at(0) + 684; be->wp_addr_shared = 1; be->meta_parsed = true;
    sample_registry(d, clock_ns); batch();
    finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
    mapping_incarnation = 2;
    CHECK(refresh_pg13_generation(d) < 0 && !be->sampled_reader.pid &&
          !be->pgbs_addr && be->sampled_cache_stale &&
          d->backend_status_layout.validation == PGWT_PGBS_VALIDATION_NOT_RUN,
          "PG13 sampler generation change retires row and wait-context caches");
    /* Model successful controlled validation of the new generation, as poll
     * requires before re-enabling this source. The VA and PM PID are reused. */
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED; pg13_anchor(511);
    d->backend_status_layout = layout;
    proc_identity(1, 1000, 1); wait_addr = proc_at(1) + 684;
    put(0, &layout.st_databaseid, 7777);
    reset_counts(); sample_registry(d, clock_ns + 1); batch();
    CHECK(targets[0].rc == 0 && be->wp_addr == wait_addr &&
          !be->sampled_cache_stale && targets[0].attr.databaseid == 7777 &&
          be->sampled_reader.shm_generation == layout.shm_generation,
          "PG13 sampler resumes only with freshly validated generation and wait context");
    release_daemon(d);
    puts("PG13 sampler loop: full pending populations, every-tick retry, generation invalidation PASS");
}

static void test_registry_paths(int major)
{
    const char *paths[] = {"fork sampled", "fork tiered sampled", "fork full",
        "fork tiered escalated", "scan before promotion", "scan after promotion",
        "INIT unknown pid", "INIT existing pid", "sampled to exact to sampled",
        "exact to sampled", "re-escalation", "scan uninitialized to fork",
        "dead slot reuse", "lazy enrollment after transient stat failure"};
    for (size_t path = 0; path < sizeof(paths)/sizeof(paths[0]); path++) {
        struct pgwt_daemon *d = new_daemon(major);
        if (path == 0) d->mode = PGWT_MODE_SAMPLED;
        if (path == 2 || path == 9) d->mode = PGWT_MODE_FULL;
        if (path == 3) d->escalation.active = d->escalation.admitting = true;
        if (path == 4) d->backend_status_layout.validation = PGWT_PGBS_VALIDATION_DEGRADED;
        if (path == 13) stat_fail = 1;
        if (path == 4 || path == 5 || path == 11) {
            wait_addr = path == 11 ? 0 : test_wait(major, 0);
            CHECK(pgwt_scan_existing_backends(d) >= 0, "production startup scan");
        } else if (path == 6) {
            CHECK(pgwt_handle_init(d, 1000, test_wait(major, 0), 0) == 0, "production unknown INIT");
        } else {
            CHECK(pgwt_handle_fork(d, 1000) == 0, "production FORK in each capture mode");
            if (path == 7) CHECK(pgwt_handle_init(d, 1000, test_wait(major, 0), 0) == 0, "production existing INIT");
            if (path == 12) {
                pgwt_handle_exit(d, 1000);
                CHECK(pgwt_handle_fork(d, 1000) == 0, "production allocator reuses dead slot");
            }
        }
        CHECK(d->backends.count == 1 && d->backends.entries[0].pid == 1000,
              "entry path registers the backend");
        /* Every scenario also proves that correctness does not need eager
         * enrollment: emulate an identity read that could not be taken then. */
        struct pgwt_backend *be = &d->backends.entries[0];
        if (path != 13) CHECK(be->sampled_reader.start_time == 1,
              "eager enrollment captures identity independently of tier and layout");
        pgwt_pgbs_reader_reset(&be->sampled_reader);
        stat_fail = 0;
        d->backend_status_layout = layout; /* late promotion */
        if (path == 8 || path == 10) {
            d->escalation.active = d->escalation.admitting = true;
            if (path == 10) {
                d->escalation.active = d->escalation.admitting = false;
                d->escalation.active = d->escalation.admitting = true;
            }
        }
        d->mode = PGWT_MODE_TIERED;
        d->escalation.active = d->escalation.admitting = false;
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
        wait_addr = test_wait(major, 0);
        reset_counts();
        CHECK(sample_registry(d, clock_ns + UINT64_C(100000000)) == 1,
              "first sampleable tick builds one target");
        batch();
        finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
        CHECK(be->sampled_reader.pid == be->pid && be->sampled_reader.start_time == 1 &&
              targets[0].rc == 0 && targets[0].attr.databaseid == 16384 &&
              own_calls == (major == 13 ? 0 : 1) && stat_reads == 2,
              "first sampled tick enrolls every registry path");
        printf("PG%d registry: %s -> first sampled tick enrolled and attributed\n", major, paths[path]);
        release_daemon(d);
    }
}
static void test_enrollment_without_attr(int major)
{
    for (int logger = 0; logger < 2; logger++) {
        struct pgwt_daemon *d = new_daemon(major);
        struct pgwt_backend *be = alloc_backend(&d->backends, 1000, major == 13);
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
        if (logger) be->meta.backend_type = PGWT_BT_LOGGER;
        else d->backend_status_layout.validation = PGWT_PGBS_VALIDATION_DEGRADED;
        reset_counts();
        CHECK(sample_registry(d, clock_ns) == 1 && be->sampled_reader.start_time == 1 &&
              stat_reads == 1 && !own_calls,
              "enrollment precedes sampling even without a tick attribution source");
        release_daemon(d);
    }
    struct pgwt_daemon *d = new_daemon(major);
    struct pgwt_backend *be = alloc_backend(&d->backends, 1000, major == 13);
    be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
    stat_fail = 1;
    CHECK(sample_registry(d, clock_ns) == 0 && !be->sampled_reader.pid,
          "failed identity read cannot create an unenrolled sample");
    for (int i = 1; i < 100; i++)
        CHECK(sample_registry(d, clock_ns + (uint64_t)i * 1000000) == 0 && stat_reads == 1,
              "failed enrollment cannot retry at sample-rate cadence");
    stat_fail = 0;
    CHECK(sample_registry(d, clock_ns + UINT64_C(100000000)) == 1 &&
          be->sampled_reader.start_time == 1 && stat_reads == 2,
          "failed enrollment recovers at its time deadline");
    release_daemon(d);
}
static void test_lifecycle_identity(int major)
{
    for (int exact = 0; exact < 4; exact++) {
        struct pgwt_daemon *d = new_daemon(major);
        if (exact == 1) d->mode = PGWT_MODE_FULL;
        if (exact >= 2) {
            d->escalation.active = true;
            d->escalation.admitting = exact == 2;
        }
        CHECK(pgwt_handle_fork(d, 1000) == 0, "identity fixture fork");
        struct pgwt_backend *be = &d->backends.entries[0];
        if (major == 13) {
            be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1;
            be->meta_parsed = true;
            sample_registry(d, clock_ns); batch();
            finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
        } else resolve_sampled_attr(d, be, clock_ns, NULL);
        uint64_t address = be->sampled_reader.status_addr;
        reset_counts();
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
        for (int tick = 0; tick < 500; tick++)
            CHECK(sample_registry(d, clock_ns + (uint64_t)tick * 1000000) == 1,
                  "resolved registry target stays sampled");
        CHECK(!own_calls && !stat_reads && map_reads == (major == 13 ? 500 : 0) &&
              be->sampled_reader.status_addr == address,
              "pid-zero guard preserves identity and cached row across sampled ticks");
        CHECK(pgwt_handle_fork(d, 1000) == 0 && be->sampled_reader.status_addr == address && !exit_calls,
              "duplicate fork preserves original identity and proof");
        be->wp_fd = 17; be->bootstrap_fd = 18;
        be->attach_ts = test_wait(major, 0); be->init_ts = 1235;
        be->databaseid = 9999; be->userid = 8888;
        be->meta.backend_type = PGWT_BT_AUTOVAC_WORKER; be->meta_parsed = true;
        incarnation = 2;
        CHECK(pgwt_handle_fork(d, 1000) == 0 && be->sampled_reader.start_time == 2 &&
              !be->sampled_reader.status_addr && exit_calls == (exact ? 0 : 1),
              "existing-entry identity check replaces stale enrollment");
        CHECK(!exact || !exit_calls, "exact duplicate fork preserves new child state-map seed");
        if (exact) {
            CHECK(be->wp_fd == 17 && be->bootstrap_fd == 18 && be->wp_addr == test_wait(major, 0) &&
                  be->attach_ts == test_wait(major, 0) && be->init_ts == 1235,
                  "replacement preserves exact watchpoints and timestamps");
            CHECK(sample_registry(d, clock_ns) == 0 && be->wp_addr == test_wait(major, 0) && be->sampled_cache_stale,
                  "replacement cannot sample predecessor wait slot during exact capture");
            if (exact >= 2) {
                d->escalation.admitting = false;
                CHECK(sample_registry(d, clock_ns) == 0 && be->sampled_cache_stale && be->wp_addr == test_wait(major, 0),
                      "deferred sampled cache invalidation waits through exact drain");
            }
        }
        d->mode = PGWT_MODE_TIERED;
        d->escalation.active = d->escalation.admitting = false;
        wait_addr = test_wait(major, 1);
        if (major == 13) proc_identity(1, 1000, 1);
        CHECK(sample_registry(d, clock_ns) == 1, "replacement becomes sampleable after teardown");
        if (exact) CHECK(be->wp_addr == test_wait(major, 1) && !be->sampled_cache_stale &&
                         be->databaseid == 0 && be->userid == 0,
                         "replacement resolves its own wait slot and discards old sampled context");
        CHECK(recorded_samples[0].backend_type == PGWT_BT_CLIENT,
              "replacement reparses metadata instead of inheriting predecessor type");
        batch();
        CHECK(targets[0].rc == 0 && be->sampled_reader.start_time == 2,
              "replacement during exact window resumes with new sampled identity");
        release_daemon(d);
    }
}
static void test_candidate_rejection(int major)
{
    struct pgwt_daemon *d = new_daemon(major);
    fixture(major, 3); d->backend_status_layout = layout;
    struct pgwt_backend *be = alloc_backend(&d->backends, 1000, major == 13);
    CHECK(pgwt_sampler_enroll_backend(be, clock_ns) == 0, "candidate fixture enrolls");
    put(0, &layout.st_state, layout.state_max + 1); /* false row hint */
    put(1, &layout.st_procpid, 1000); /* the real row */
    if (major == 13) {
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1;
        be->meta_parsed = true; sample_registry(d, clock_ns);
    } else resolve_sampled_attr(d, be, clock_ns, NULL);
    targets[0].reader = &be->sampled_reader; batch();
    finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
    CHECK(targets[0].rc == -1 && !be->sampled_reader.status_addr && stat_reads == 1,
          "an initially rejected candidate never becomes a cached row");
    entries[0] = entries[1];
    if (major == 13) {
        proc_identity(0, 1000, 2);
        sample_registry(d, clock_ns + UINT64_C(100000000));
    } else resolve_sampled_attr(d, be, clock_ns + UINT64_C(100000000), NULL);
    batch(); finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
    CHECK(targets[0].rc == 0 && be->sampled_reader.status_addr == entries[1] &&
          !be->sampled_reader.candidate && !be->pgbs_resolve_fast_until_ns && stat_reads == 2,
          "candidate rejection continues search past a false PID hint");
    release_daemon(d);
}
static void test_reuse_without_events(int major)
{
    /* Reviewer's scenario: EXIT and the successor's FORK are both dropped.
     * The replacement has the same pid, a different start time and row, and
     * a different wait slot. No lifecycle callback repairs the registry. */
    struct pgwt_daemon *d = new_daemon(major);
    fixture(major, 2); d->backend_status_layout = layout;
    CHECK(pgwt_handle_fork(d, 1000) == 0, "reuse fixture fork");
    struct pgwt_backend *be = &d->backends.entries[0];
    be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
    wait_addr = test_wait(major, 0);
    put(1, &layout.st_procpid, 0);
    CHECK(sample_registry(d, clock_ns) == 1, "reuse original target");
    batch(); finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
    CHECK(targets[0].rc == 0 && be->sampled_reader.start_time == 1,
          "reuse original accepted before unseen exit");
    incarnation = 2;
    put(0, &layout.st_procpid, 0);
    put(1, &layout.st_procpid, 1000); put(1, &layout.st_databaseid, 7777);
    entries[0] = (uintptr_t)row_at(1);
    if (major == 13) { proc_identity(0, 1000, 2); proc_identity(1, 1000, 2); }
    wait_addr = test_wait(major, 1);
    be->meta.backend_type = PGWT_BT_PARALLEL_WORKER;
    uint64_t t = clock_ns + UINT64_C(1000000);
    sample_registry(d, t); batch();
    finish_sampled_attr(be, targets[0].rc, targets[0].error, t);
    CHECK(targets[0].rc == -1 && !be->sampled_reader.status_addr,
          "reuse invalidates predecessor row before re-resolution");
    reset_counts();
    sample_registry(d, t + UINT64_C(100000000)); batch();
    finish_sampled_attr(be, targets[0].rc, targets[0].error, t + UINT64_C(100000000));
    CHECK(targets[0].rc == -1 && targets[0].error == ESTALE && tick_failures == 1 &&
          !targets[0].attr.databaseid && !targets[0].attr.cmd_open &&
          !targets[0].gate.cmd_open && !be->sampled_reader.pid &&
          be->sampled_cache_stale && stat_reads == 1,
          "unseen pid reuse is refused at first candidate acceptance");
    printf("PG%d REUSE: +100ms successor refused; enrollment retired, failure=1\n", major);
    sample_registry(d, t + UINT64_C(200000000)); batch();
    finish_sampled_attr(be, targets[0].rc, targets[0].error, t + UINT64_C(200000000));
    CHECK(targets[0].rc == 0 && be->sampled_reader.start_time == 2 &&
          targets[0].attr.databaseid == 7777 && be->wp_addr == test_wait(major, 1) &&
          recorded_samples[0].wait_event_addr == test_wait(major, 1) &&
          recorded_samples[0].backend_type == PGWT_BT_CLIENT &&
          !be->sampled_cache_stale && stat_reads == 3,
          "successor is sampled only with its own enrollment wait slot and type");
    printf("PG%d REUSE: +200ms new enrollment start_time=%llu, process start_time=%d, databaseid=%u, wait_addr=%llu\n",
           major, (unsigned long long)be->sampled_reader.start_time, incarnation,
           targets[0].attr.databaseid, (unsigned long long)be->wp_addr);
    reset_counts();
    for (int i = 0; i < 100; i++) { sample_registry(d, t + UINT64_C(300000000)); batch(); }
    CHECK(!stat_reads && !own_calls, "accepted successor has no per-tick identity I/O");
    release_daemon(d);
}
static void test_refusal_tick(int major)
{
    const enum pgwt_backend_type types[] = {PGWT_BT_PARALLEL_WORKER, PGWT_BT_CLIENT};
    const uint32_t waits[] = {0, 0x0a000001}; /* CPU and IO:DataFileRead */
    for (size_t type = 0; type < sizeof(types)/sizeof(types[0]); type++)
    for (int tier = 0; tier < 4; tier++)
    for (size_t w = 0; w < sizeof(waits)/sizeof(waits[0]); w++) {
        struct pgwt_daemon *d = new_daemon(major);
        fixture(major, 2); d->backend_status_layout = layout;
        d->mode = tier == 0 ? PGWT_MODE_SAMPLED :
                  tier == 1 ? PGWT_MODE_FULL : PGWT_MODE_TIERED;
        d->escalation.active = tier == 3;
        CHECK(pgwt_handle_fork(d, 1000) == 0, "refusal recording fixture fork");
        struct pgwt_backend *be = &d->backends.entries[0];
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
        be->meta.backend_type = types[type];
        wait_addr = refusal_wait_addr = test_wait(major, 0); refusal_wait_value = waits[w];
        put(1, &layout.st_procpid, 0);
        CHECK(record_registry_tick(d, clock_ns) == 1,
              "original incarnation records before unseen exit");
        incarnation = 2;
        put(0, &layout.st_procpid, 0);
        put(1, &layout.st_procpid, 1000);
        entries[0] = (uintptr_t)row_at(1);
        if (major == 13) { proc_identity(0, 1000, 2); proc_identity(1, 1000, 2); }
        wait_addr = test_wait(major, 1);
        uint64_t t = clock_ns + UINT64_C(1000000);
        (void)record_registry_tick(d, t); /* cached row rejected; next tick resolves */
        reset_counts();
        int count = record_registry_tick(d, t + UINT64_C(100000000));
        printf("PG%d type=%d tier=%d we=%u refusal: rc=%d err=%s recorded=%s wait_addr=%llu\n",
               major, types[type], tier, waits[w], targets[0].rc,
               targets[0].error == ESTALE ? "ESTALE" : "OTHER",
               count ? "YES" : "NO",
               (unsigned long long)recorded_samples[0].wait_event_addr);
        CHECK(targets[0].rc == -1 && targets[0].error == ESTALE && tick_failures == 1,
              "recording probe refuses stale identity exactly once");
        CHECK(count == 0 && !d->sampler->read_valid[0],
              "stale identity refusal records zero samples for every type and tier");
        /* Exact capture defers cache replacement until teardown completes. */
        d->mode = PGWT_MODE_SAMPLED; d->escalation.active = false;
        refusal_wait_addr = test_wait(major, 1);
        count = record_registry_tick(d, t + UINT64_C(200000000));
        CHECK(count == 1 && d->sampler->samples[0].pid == 1000 &&
              d->sampler->samples[0].new_event == waits[w] &&
              recorded_samples[0].wait_event_addr == test_wait(major, 1) &&
              be->sampled_reader.start_time == 2 && tick_failures == 1,
              "next accepted tick records successor with its own wait slot");
        release_daemon(d);
    }
    refusal_wait_addr = 0;
}
static void test_init_reuse(int major)
{
    for (int exact = 0; exact < 2; exact++) {
        struct pgwt_daemon *d = new_daemon(major);
        CHECK(pgwt_handle_fork(d, 1000) == 0, "INIT reuse fork");
        struct pgwt_backend *be = &d->backends.entries[0];
        be->wp_addr = test_wait(major, 0); be->wp_addr_shared = 1; be->meta_parsed = true;
        sample_registry(d, clock_ns); batch();
        finish_sampled_attr(be, targets[0].rc, targets[0].error, clock_ns);
        be->wp_fd = 17; be->bootstrap_fd = -1;
        incarnation = 2;
        d->mode = exact ? PGWT_MODE_FULL : PGWT_MODE_TIERED;
        reset_counts();
        int init_rc = pgwt_handle_init(d, 1000, test_wait(major, 1), 55);
        CHECK(init_rc == 0 &&
              be->sampled_reader.start_time == 2 && !be->sampled_reader.status_addr &&
              be->sampled_cache_stale && be->wp_fd == 17 && !exit_calls && stat_reads == 3,
              "INIT checks existing identity before the already-attached return");
        release_daemon(d);
    }
}
static void test_rejected_backoff(int major)
{
    /* Drive the production target loop AND production post-batch policy. The
     * earlier harness called only finish(), omitting poll's deadline reset. */
    const int expected[] = {0,100,200,300,400,500,600,700,800,900,1000,1100,
                            1200,1400,1800,2600,4200,7400,10600};
    struct pgwt_daemon *d = new_daemon(major);
    CHECK(pgwt_handle_fork(d, 1000) == 0, "rejected backoff fork");
    struct pgwt_backend *be = &d->backends.entries[0];
    be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
    put(0, &layout.st_state, layout.state_max + 1);
    int attempts = 0;
    reset_counts();
    printf("PG%d rejected-candidate attempt ms:", major);
    for (int ms = 0; ms <= 10600; ms++) {
        uint64_t t = clock_ns + (uint64_t)ms * UINT64_C(1000000);
        /* PG13's scan cursor wraps while retaining the same rejected row. */
        if (major == 13) be->sampled_reader.scan_cursor = 0;
        int before = own_calls;
        sample_registry(d, t); batch();
        finish_sampled_attr(be, targets[0].rc, targets[0].error, t);
        if (own_calls == before) continue;
        CHECK(own_calls == before + 1 && attempts < (int)(sizeof(expected)/sizeof(*expected)) &&
              ms == expected[attempts], "rejected candidates preserve exponential attempt times");
        attempts++;
        printf(" %d", ms);
    }
    puts("");
    CHECK(attempts == (int)(sizeof(expected)/sizeof(*expected)) && !stat_reads,
          "rejected candidates do no acceptance stat reads and cap at 3200ms");
    release_daemon(d);
}
static void test_first_tick_budget(int major)
{
    /* Already-proven wait slots and process titles, as in the reviewer's
     * setup count. This includes ALL attribution setup and the real wait
     * batch, with no hidden stdio fstat/buffering calls in the product.
     * For general maps files let R be the sum of map read calls (including
     * EOF): total = 4505 + R. Here each of 501 maps files fits one 4096-byte
     * read plus EOF, so R=1002 and total=5507. Larger maps add read calls,
     * never opens; interrupted syscalls are outside this successful-I/O bound. */
    struct pgwt_daemon *d = new_daemon(major);
    fixture(major, 500); d->backend_status_layout = layout;
    d->my_be_entry_addr = UINT64_C(0x1234000);
    for (int i = 0; i < 500; i++) {
        struct pgwt_backend *be = alloc_backend(&d->backends, 1000+i, major == 13);
        be->wp_addr = major == 13 ? proc_at(i) + 684 : (uintptr_t)&wait_words[i]; be->wp_addr_shared = 1; be->meta_parsed = true;
    }
    CHECK(sample_registry(d, clock_ns) == 500, "500 first-tick registry targets");
    batch();
    for (int i = 0; i < 500; i++) {
        CHECK(targets[i].rc == 0, "all first-tick candidates pass batch identity");
        finish_sampled_attr(&d->backends.entries[i], targets[i].rc, targets[i].error, clock_ns);
    }
    uint32_t vals[500]; uint8_t valid[500]; uint64_t faults = 0;
    CHECK(pgwt_sampler_read_targets(recorded_samples, 500, vals, valid, &faults) == 500 &&
          wait_calls == 1 && !faults, "first tick includes one real wait batch");
    CHECK(own_calls + calls + wait_calls + proc_opens + proc_reads + proc_closes == (major == 13 ? 5013 : 5507),
          "N=500 complete first-tick syscall bound includes file setup and wait batch");
    CHECK(own_calls == (major == 13 ? 0 : 500) && calls == (major == 13 ? 4 : 2) &&
          stat_reads == 1000 && map_reads == (major == 13 ? 502 : 501) &&
          reader_map_reads == (major == 13 ? 2 : 1) && proc_opens == (major == 13 ? 1502 : 1501) &&
          proc_reads == (major == 13 ? 2004 : 2002) && proc_closes == (major == 13 ? 1502 : 1501) &&
          !opens && !openats && !reads && !closes,
          "N=500 first tick includes enrollment, acceptance and both mapping proofs");
    if (major == 13)
        puts("PG13 first tick N=500: own VM=0 discovery VM=2 status VM=2 wait VM=1; stat=1000 maps=502; open/read/close=1502/2004/1502; total=5013");
    else printf("PG%d first tick N=500 (maps <=4096 bytes): own VM=500 status VM=2 + wait VM=1; stat=1000 maps=501 (reader=1); open/read/close=1501/2002/1501; total=5507 (general: 4505 + maps read calls)\n", major);
    reset_counts();
    CHECK(sample_registry(d, clock_ns + UINT64_C(1000000)) == 500, "500 cached targets");
    batch();
    CHECK(pgwt_sampler_read_targets(recorded_samples, 500, vals, valid, &faults) == 500 &&
          wait_calls == 1 && !faults, "steady tick includes one wait batch");
    CHECK(!own_calls && !stat_reads && map_reads == (major == 13 ? 1 : 0) && calls == 2 && wait_calls == 1 &&
          proc_opens == (major == 13 ? 1 : 0) && proc_reads == (major == 13 ? 2 : 0) &&
          proc_closes == (major == 13 ? 1 : 0),
          "N=500 resolved second tick does no enrollment or discovery I/O");
    release_daemon(d);
}
static void test_tick_maps_cache(int major)
{
    for (int outcome = 0; outcome < 3; outcome++) {
        fixture(major, 3);
        struct pgwt_pgbs_maps_cache maps_cache = {0};
        struct pgwt_pgbs_reader readers[3] = {0};
        maps_fail = outcome == 1;
        private_reader = outcome == 2;
        for (int i = 0; i < 3; i++) {
            CHECK(pgwt_pgbs_reader_enroll(&readers[i], 1000+i) == 0,
                  "tick mapping fixture enrolls");
            int rc = pgwt_pgbs_reader_resolve(&readers[i], 42,
                         (uintptr_t)&entries[i], &layout, &maps_cache);
            CHECK((rc == 0) == (outcome == 0),
                  "shared tick snapshot keeps failed and private reader proofs closed");
        }
        CHECK(reader_map_reads == 1 && map_reads == 4,
              "postmaster maps are opened at most once per tick even after failure");
        pgwt_pgbs_maps_cache_reset(&maps_cache);
        for (int i = 0; i < 3; i++) pgwt_pgbs_reader_reset(&readers[i]);
        /* Next tick: a fresh snapshot must see the changed reader proof. */
        maps_fail = 0; private_reader = !private_reader;
        CHECK(pgwt_pgbs_reader_enroll(&readers[0], 1000) == 0,
              "next tick mapping fixture enrolls");
        int rc = pgwt_pgbs_reader_resolve(&readers[0], 42,
                     (uintptr_t)&entries[0], &layout, &maps_cache);
        CHECK(reader_map_reads == 2 && (rc == 0) == !private_reader,
              "tick cache invalidation observes the next reader mapping snapshot");
        pgwt_pgbs_reader_reset(&readers[0]);
        pgwt_pgbs_maps_cache_reset(&maps_cache);
    }
    fixture(major, 3);
    maps_prefix_lines = 128; /* shared lines follow the 4096-byte read boundary */
    struct pgwt_pgbs_maps_cache maps_cache = {0};
    struct pgwt_pgbs_reader readers[3] = {0};
    for (int i = 0; i < 3; i++) {
        CHECK(pgwt_pgbs_reader_enroll(&readers[i], 1000+i) == 0 &&
              pgwt_pgbs_reader_resolve(&readers[i], 42, (uintptr_t)&entries[i],
                                       &layout, &maps_cache) == 0,
              "mapping proof reads shared lines beyond the first file chunk");
        targets[i].reader = &readers[i];
    }
    batch();
    CHECK(map_reads == 4 && reader_map_reads == 1 && stat_reads == 6 &&
          proc_opens == 10 && proc_reads == 18 && proc_closes == 10 &&
          targets[0].rc == 0 && targets[1].rc == 0 && targets[2].rc == 0,
          "complete setup count includes extra maps read chunks and acceptance stat");
    for (int i = 0; i < 3; i++) pgwt_pgbs_reader_reset(&readers[i]);
    pgwt_pgbs_maps_cache_reset(&maps_cache);
}
static void test_pending_budget(int major)
{
    struct pgwt_daemon *d = new_daemon(major);
    /* Fixed pending population. Successful enrollments/maps are setup, not
     * recurring work; the next one second is replayed at 10 and 1000 Hz. */
    fixture(major, 500); d->backend_status_layout = layout;
    for (int hz = 10; hz <= 1000; hz *= 100) {
        struct pgwt_pgbs_maps_cache maps_cache = {0};
        for (int i = 0; i < 500; i++) {
            struct pgwt_backend *be = alloc_backend(&d->backends, 1000+i, major == 13);
            CHECK(be != NULL, "pending registry entry");
            CHECK(pgwt_sampler_enroll_backend(be, clock_ns) == 0, "pending enrollment");
            if (major == 13) put(i, &layout.st_procpid, 0);
            else entries[i] = 0;
            d->my_be_entry_addr = (uintptr_t)&entries[i];
            resolve_sampled_attr(d, be, clock_ns, &maps_cache);
        }
        pgwt_pgbs_maps_cache_reset(&maps_cache);
        CHECK(stat_reads == 500 && map_reads == 501 && reader_map_reads == 1 &&
              own_calls == 500 && proc_opens == 1001 && proc_reads == 1502 &&
              proc_closes == 1001 && !opens && !reads && !closes,
              "pending setup reads identity and shared reader maps once");
        reset_counts();
        int max_due = 0;
        for (int tick = 1; tick <= hz; tick++) {
            int before = own_calls;
            for (int i = 0; i < 500; i++) {
                d->my_be_entry_addr = (uintptr_t)&entries[i];
                resolve_sampled_attr(d, &d->backends.entries[i],
                    clock_ns + (uint64_t)tick * UINT64_C(1000000000) / (unsigned)hz, NULL);
            }
            if (own_calls - before > max_due) max_due = own_calls - before;
        }
        CHECK(own_calls == 5000 && max_due == 500 && !stat_reads && !map_reads &&
              !opens && !openats && !reads && !closes && !proc_opens && !proc_reads && !proc_closes,
              "N=500 pending cost is one VM per due backend independent of tick rate");
        printf("PG%d pending N=500 Hz=%d: max VM/tick=%d, VM/second=%d, stat=0 maps=0 mem open/pread/close=0 (after setup)\n",
               major, hz, max_due, own_calls);
        for (int i = 0; i < d->backends.count; i++) {
            pgwt_pgbs_reader_reset(&d->backends.entries[i].sampled_reader);
            free(d->backends.entries[i].pg13_synthetic_cache);
        }
        memset(&d->backends, 0, sizeof(d->backends)); reset_counts();
    }
    release_daemon(d);
}
static void test_retry_and_idle(int major)
{
    struct pgwt_daemon *d = new_daemon(major);
    CHECK(pgwt_handle_fork(d, 1000) == 0, "retry fixture fork");
    struct pgwt_backend *be = &d->backends.entries[0];
    uint64_t original = entries[0];
    if (major == 13) put(0, &layout.st_procpid, 0); else entries[0] = 0;
    for (int i = 0; i < 5; i++) resolve_sampled_attr(d, be, clock_ns + (uint64_t)i * 100000000, NULL);
    CHECK(own_calls == 5 && !be->sampled_reader.status_addr,
          "startup attempts follow 100 ms cadence");
    if (major == 13) put(0, &layout.st_procpid, 1000); else entries[0] = original;
    resolve_sampled_attr(d, be, clock_ns + UINT64_C(450000000), NULL);
    CHECK(own_calls == 5, "late row waits for time deadline rather than next tick");
    resolve_sampled_attr(d, be, clock_ns + UINT64_C(500000000), NULL);
    targets[0].reader = &be->sampled_reader; batch();
    CHECK(targets[0].rc == 0, "late status row resolves at next 100 ms deadline");
    /* Drop the candidate as the batch would after slot reuse. */
    be->sampled_reader.status_addr = 0;
    if (major == 13) put(0, &layout.st_procpid, 0); else entries[0] = 0;
    for (int i = 6; i <= 12; i++) resolve_sampled_attr(d, be, clock_ns + (uint64_t)i * 100000000, NULL);
    CHECK(be->pgbs_resolve_retry_ns == clock_ns + UINT64_C(1400000000),
          "one-second startup window transitions to exponential backoff");
    for (int i = 0; i < 10; i++) {
        uint64_t tick = be->pgbs_resolve_retry_ns;
        resolve_sampled_attr(d, be, tick, NULL);
        CHECK(be->pgbs_resolve_retry_ns - tick <= UINT64_C(3200000000),
              "mature unreadable target backoff stays capped");
    }
    struct pgwt_sample_target target = {.pid = 1000, .backend_type = PGWT_BT_CLIENT,
        .databaseid = 16384, .userid = 10};
    note_idle_attr_sample(d, &target, true, true, WEI(PG_WAIT_CLIENT, 0));
    note_idle_attr_sample(d, &target, true, false, WEI(PG_WAIT_CLIENT, 0));
    CHECK(d->counters.sampled_attr_tick_idle_samples_total == 1,
          "idle attribution counter requires successful status and wait reads");
    reset_counts();
    CHECK(setenv("PGWT_TEST_DISABLE_STATUS_BATCH", "1", 1) == 0, "disabled batch fixture");
    batch();
    CHECK(unsetenv("PGWT_TEST_DISABLE_STATUS_BATCH") == 0, "clear disabled batch fixture");
    CHECK(calls == 0 && targets[0].rc == -1 && tick_failures == 1,
          "disabled status batch issues no calls and leaves attribution unread");
    release_daemon(d);
}

#endif
int main(int argc, char **argv)
{
    (void)io_openat; /* catches an accidentally reintroduced openat */
    if (argc == 2) {
        if (!strcmp(argv[1], "pg13")) {
            test_pg13_computed(); test_pg13_guards();
            test_pg13_pending_backend_id_known_base(); test_pg13_rejected_computed_hint();
        }
        else if (!strcmp(argv[1], "faults")) test_faults(18);
        else if (!strcmp(argv[1], "budget")) test_budget(18, 512, 2);
        else if (!strcmp(argv[1], "resolve")) test_resolve(18);
#ifdef PGWT_LIFECYCLE_SOURCE
        else if (!strcmp(argv[1], "pg13-registry")) test_pg13_registry();
        else if (!strcmp(argv[1], "registry")) { test_registry_paths(18); test_enrollment_without_attr(18); }
        else if (!strcmp(argv[1], "identity")) test_lifecycle_identity(18);
        else if (!strcmp(argv[1], "reuse")) test_reuse_without_events(18);
        else if (!strcmp(argv[1], "refusal")) test_refusal_tick(18);
        else if (!strcmp(argv[1], "init-reuse")) test_init_reuse(18);
        else if (!strcmp(argv[1], "rejected-backoff")) test_rejected_backoff(18);
        else if (!strcmp(argv[1], "first-tick")) test_first_tick_budget(18);
        else if (!strcmp(argv[1], "pending")) test_pending_budget(18);
        else if (!strcmp(argv[1], "retry")) test_retry_and_idle(18);
#endif
        else return 2;
    } else {
        test_pg13_computed(); test_pg13_guards();
        test_pg13_pending_backend_id_known_base(); test_pg13_rejected_computed_hint();
#ifdef PGWT_LIFECYCLE_SOURCE
        test_pg13_registry();
#endif
        for (int i = 0; i < 3; i++) {
        const int major[] = {13,17,18};
        test_budget(major[i], 512, 2);
        test_budget(major[i], 1024, major[i] == 13 ? 4 : 3);
        test_faults(major[i]);
        if (major[i] != 13) test_resolve(major[i]);
#ifdef PGWT_LIFECYCLE_SOURCE
        test_registry_paths(major[i]);
        test_enrollment_without_attr(major[i]);
        test_lifecycle_identity(major[i]);
        test_candidate_rejection(major[i]);
        test_reuse_without_events(major[i]);
        test_refusal_tick(major[i]);
        test_init_reuse(major[i]);
        test_first_tick_budget(major[i]);
        if (major[i] != 13) {
            test_rejected_backoff(major[i]);
            test_tick_maps_cache(major[i]);
            test_pending_budget(major[i]);
            test_retry_and_idle(major[i]);
        }
#endif
    }
    }
    for (size_t i = 0; i < 1024; i++) pgwt_pgbs_reader_reset(&cache[i]);
    printf("sampled_attr_io: %d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}
