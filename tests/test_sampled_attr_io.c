/* Inject libc and process_vm_readv into the production translation unit.
 * Runs on macOS: no /proc, PostgreSQL, BPF, timing, or kernel assumptions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
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
static int own_calls, stat_reads, map_reads, stat_fail, own_short;
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
        stat_reads++;
        if (stat_fail) { fclose(fp); errno = ENOENT; return NULL; }
        fputs("1000 (postgres) S 42", fp);
        for (int field = 5; field < 22; field++) fputs(" 0", fp);
        fprintf(fp, " %d\n", incarnation);
    } else {
        map_reads++;
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
static void reset_counts(void)
{
    opens = openats = closes = reads = calls = max_iovs = 0;
    tick_failures = 0; fopens = own_calls = stat_reads = map_reads = 0;
    memset(visits, 0, sizeof(visits)); memset(after_copy, 0, sizeof(after_copy));
}
static void fixture(int major, size_t n)
{
    for (size_t i = 0; i < 1024; i++) pgwt_pgbs_reader_reset(&cache[i]);
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
    incarnation = 1; stat_fail = own_short = 0; tear_row = short_row = fault_row = -1;
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
    struct pgwt_pgbs_reader r = {0};
    CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0 && r.start_time == 1 && stat_reads == 1,
          "enrollment stores start time without descriptor");
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == 0 &&
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
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == 0 &&
          own_calls == 1 && !stat_reads && !map_reads,
          "invalidated candidate reuses mapping snapshots");
    reset_counts();
    for (int tick = 0; tick < 500; tick++)
        CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == 0,
              "cached row resolver succeeds");
    CHECK(!own_calls && !map_reads && !stat_reads,
          "cached-address return prevents re-resolution on every tick");
    batch(); CHECK(targets[1].rc == 0, "re-resolved candidate is accepted by batch");
    pgwt_pgbs_reader_reset(&r);
    for (int reader_private = 0; reader_private < 2; reader_private++) {
        CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0, "mapping fixture enrolls");
        private_target = !reader_private; private_reader = reader_private;
        CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == -1 &&
              !r.shared && !r.status_addr, "both mappings must prove shared");
        pgwt_pgbs_reader_reset(&r);
    }
    private_target = private_reader = 0;
    CHECK(pgwt_pgbs_reader_enroll(&r, 1001) == 0, "short-pointer fixture enrolls");
    own_short = 1;
    CHECK(pgwt_pgbs_reader_resolve(&r, 42, (uintptr_t)&entries[1], &layout) == -1 &&
          !r.status_addr, "short own-pid read never supplies a candidate");
    own_short = 0;
    pgwt_pgbs_reader_reset(&r); CHECK(!r.pid && !r.target_maps, "retirement clears row and mapping proof");
    fixture(major, 3); reset_counts();
    pgwt_pgbs_read_batch(42, &layout, targets, population, true, &tick_failures);
    CHECK(calls == 1 && targets[1].rc == 0 && targets[1].gate.cmd_open &&
          targets[1].gate.query_id_valid == (major != 13) && !opens && !reads,
          "command recheck uses one foreign-pid batch and same predicate");
}

#ifdef PGWT_LIFECYCLE_SOURCE
/* Only hardware/BPF boundaries are fakes. The Python runner extracts and
 * compiles the production allocation, FORK, enrollment and retry functions;
 * /proc and VM reads still use the same injection layer above. */
#include "daemon.h"
#include "backend_init.h"
#include "idle_rule.h"
#include "discovery.h"
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
#include PGWT_LIFECYCLE_SOURCE

#undef opendir
#undef readdir
#undef closedir
#undef fopen

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
            wait_addr = path == 11 ? 0 : 1234;
            CHECK(pgwt_scan_existing_backends(d) >= 0, "production startup scan");
        } else if (path == 6) {
            CHECK(pgwt_handle_init(d, 1000, 1234, 0) == 0, "production unknown INIT");
        } else {
            CHECK(pgwt_handle_fork(d, 1000) == 0, "production FORK in each capture mode");
            if (path == 7) CHECK(pgwt_handle_init(d, 1000, 1234, 0) == 0, "production existing INIT");
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
        be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
        wait_addr = 1234;
        reset_counts();
        CHECK(sample_registry(d, clock_ns + UINT64_C(100000000)) == 1,
              "first sampleable tick builds one target");
        batch();
        finish_sampled_attr(be, targets[0].rc);
        CHECK(be->sampled_reader.pid == be->pid && be->sampled_reader.start_time == 1 &&
              targets[0].rc == 0 && targets[0].attr.databaseid == 16384 &&
              own_calls == 1 && stat_reads == 1,
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
        be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
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
    be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
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
        resolve_sampled_attr(d, be, clock_ns);
        uint64_t address = be->sampled_reader.status_addr;
        reset_counts();
        be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
        for (int tick = 0; tick < 500; tick++)
            CHECK(sample_registry(d, clock_ns + (uint64_t)tick * 1000000) == 1,
                  "resolved registry target stays sampled");
        CHECK(!own_calls && !stat_reads && !map_reads && be->sampled_reader.status_addr == address,
              "pid-zero guard preserves identity and cached row across sampled ticks");
        CHECK(pgwt_handle_fork(d, 1000) == 0 && be->sampled_reader.status_addr == address && !exit_calls,
              "duplicate fork preserves original identity and proof");
        be->wp_fd = 17; be->bootstrap_fd = 18;
        be->attach_ts = 1234; be->init_ts = 1235;
        be->databaseid = 9999; be->userid = 8888;
        incarnation = 2;
        CHECK(pgwt_handle_fork(d, 1000) == 0 && be->sampled_reader.start_time == 2 &&
              !be->sampled_reader.status_addr && exit_calls == (exact ? 0 : 1),
              "existing-entry identity check replaces stale enrollment");
        CHECK(!exact || !exit_calls, "exact duplicate fork preserves new child state-map seed");
        if (exact) {
            CHECK(be->wp_fd == 17 && be->bootstrap_fd == 18 && be->wp_addr == 1234 &&
                  be->attach_ts == 1234 && be->init_ts == 1235,
                  "replacement preserves exact watchpoints and timestamps");
            CHECK(sample_registry(d, clock_ns) == 0 && be->wp_addr == 1234 && be->sampled_cache_stale,
                  "replacement cannot sample predecessor wait slot during exact capture");
            if (exact >= 2) {
                d->escalation.admitting = false;
                CHECK(sample_registry(d, clock_ns) == 0 && be->sampled_cache_stale && be->wp_addr == 1234,
                      "deferred sampled cache invalidation waits through exact drain");
            }
        }
        d->mode = PGWT_MODE_TIERED;
        d->escalation.active = d->escalation.admitting = false;
        wait_addr = 2345;
        CHECK(sample_registry(d, clock_ns) == 1, "replacement becomes sampleable after teardown");
        if (exact) CHECK(be->wp_addr == 2345 && !be->sampled_cache_stale &&
                         be->databaseid == 0 && be->userid == 0,
                         "replacement resolves its own wait slot and discards old sampled context");
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
    resolve_sampled_attr(d, be, clock_ns);
    targets[0].reader = &be->sampled_reader; batch();
    finish_sampled_attr(be, targets[0].rc);
    CHECK(targets[0].rc == -1 && !be->sampled_reader.status_addr,
          "an initially rejected candidate never becomes a cached row");
    entries[0] = entries[1];
    resolve_sampled_attr(d, be, clock_ns + UINT64_C(100000000));
    batch(); finish_sampled_attr(be, targets[0].rc);
    CHECK(targets[0].rc == 0 && be->sampled_reader.status_addr == entries[1] &&
          !be->sampled_reader.candidate && !be->pgbs_resolve_fast_until_ns,
          "candidate rejection continues search past a false PID hint");
    release_daemon(d);
}
static void test_first_tick_budget(int major)
{
    struct pgwt_daemon *d = new_daemon(major);
    fixture(major, 500); d->backend_status_layout = layout;
    d->my_be_entry_addr = UINT64_C(0x1234000);
    for (int i = 0; i < 500; i++) {
        struct pgwt_backend *be = alloc_backend(&d->backends, 1000+i, major == 13);
        be->wp_addr = 1234; be->wp_addr_shared = 1; be->meta_parsed = true;
    }
    CHECK(sample_registry(d, clock_ns) == 500, "500 first-tick registry targets");
    batch();
    for (int i = 0; i < 500; i++) {
        CHECK(targets[i].rc == 0, "all first-tick candidates pass batch identity");
        finish_sampled_attr(&d->backends.entries[i], targets[i].rc);
    }
    CHECK(own_calls == 500 && calls == 2 && stat_reads == 500 && map_reads == 1000 &&
          !opens && !openats && !reads && !closes,
          "N=500 first tick is 500 candidate calls plus two status calls and one-time setup");
    printf("PG%d first tick N=500: candidate VM=500 status VM=2; setup stat=500 maps=1000; mem open/pread/close=0\n", major);
    reset_counts();
    CHECK(sample_registry(d, clock_ns + UINT64_C(1000000)) == 500, "500 cached targets");
    batch();
    CHECK(!own_calls && !stat_reads && !map_reads && calls == 2,
          "N=500 resolved second tick does no enrollment or resolution I/O");
    release_daemon(d);
}
static void test_pending_budget(int major)
{
    struct pgwt_daemon *d = new_daemon(major);
    /* Fixed pending population. Successful enrollments/maps are setup, not
     * recurring work; the next one second is replayed at 10 and 1000 Hz. */
    fixture(major, 500); d->backend_status_layout = layout;
    for (int hz = 10; hz <= 1000; hz *= 100) {
        for (int i = 0; i < 500; i++) {
            struct pgwt_backend *be = alloc_backend(&d->backends, 1000+i, major == 13);
            CHECK(be != NULL, "pending registry entry");
            CHECK(pgwt_sampler_enroll_backend(be, clock_ns) == 0, "pending enrollment");
            if (major == 13) put(i, &layout.st_procpid, 0);
            else entries[i] = 0;
            d->my_be_entry_addr = (uintptr_t)&entries[i];
            resolve_sampled_attr(d, be, clock_ns);
        }
        CHECK(stat_reads == 500 && map_reads == 1000 && own_calls == 500 && !opens && !reads && !closes,
              "pending setup reads identity and mapping pair once");
        reset_counts();
        int max_due = 0;
        for (int tick = 1; tick <= hz; tick++) {
            int before = own_calls;
            for (int i = 0; i < 500; i++) {
                d->my_be_entry_addr = (uintptr_t)&entries[i];
                resolve_sampled_attr(d, &d->backends.entries[i],
                    clock_ns + (uint64_t)tick * UINT64_C(1000000000) / (unsigned)hz);
            }
            if (own_calls - before > max_due) max_due = own_calls - before;
        }
        CHECK(own_calls == 5000 && max_due == 500 && !stat_reads && !map_reads &&
              !opens && !openats && !reads && !closes,
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
    for (int i = 0; i < 5; i++) resolve_sampled_attr(d, be, clock_ns + (uint64_t)i * 100000000);
    CHECK(own_calls == 5 && !be->sampled_reader.status_addr,
          "startup attempts follow 100 ms cadence");
    if (major == 13) put(0, &layout.st_procpid, 1000); else entries[0] = original;
    resolve_sampled_attr(d, be, clock_ns + UINT64_C(450000000));
    CHECK(own_calls == 5, "late row waits for time deadline rather than next tick");
    resolve_sampled_attr(d, be, clock_ns + UINT64_C(500000000));
    targets[0].reader = &be->sampled_reader; batch();
    CHECK(targets[0].rc == 0, "late status row resolves at next 100 ms deadline");
    /* Drop the candidate as the batch would after slot reuse. */
    be->sampled_reader.status_addr = 0;
    if (major == 13) put(0, &layout.st_procpid, 0); else entries[0] = 0;
    for (int i = 6; i <= 12; i++) resolve_sampled_attr(d, be, clock_ns + (uint64_t)i * 100000000);
    CHECK(be->pgbs_resolve_retry_ns == clock_ns + UINT64_C(1400000000),
          "one-second startup window transitions to exponential backoff");
    for (int i = 0; i < 10; i++) {
        uint64_t tick = be->pgbs_resolve_retry_ns;
        resolve_sampled_attr(d, be, tick);
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
        if (!strcmp(argv[1], "faults")) test_faults(18);
        else if (!strcmp(argv[1], "budget")) test_budget(18, 512, 2);
        else if (!strcmp(argv[1], "resolve")) test_resolve(18);
#ifdef PGWT_LIFECYCLE_SOURCE
        else if (!strcmp(argv[1], "registry")) { test_registry_paths(18); test_enrollment_without_attr(18); }
        else if (!strcmp(argv[1], "identity")) test_lifecycle_identity(18);
        else if (!strcmp(argv[1], "pending")) test_pending_budget(18);
        else if (!strcmp(argv[1], "retry")) test_retry_and_idle(18);
#endif
        else return 2;
    } else for (int i = 0; i < 3; i++) {
        const int major[] = {13,17,18};
        test_budget(major[i], 512, 2);
        test_budget(major[i], 1024, major[i] == 13 ? 4 : 3);
        test_faults(major[i]); test_resolve(major[i]);
#ifdef PGWT_LIFECYCLE_SOURCE
        test_registry_paths(major[i]);
        test_enrollment_without_attr(major[i]);
        test_lifecycle_identity(major[i]);
        test_candidate_rejection(major[i]);
        test_first_tick_budget(major[i]);
        test_pending_budget(major[i]);
        test_retry_and_idle(major[i]);
#endif
    }
    for (size_t i = 0; i < 1024; i++) pgwt_pgbs_reader_reset(&cache[i]);
    printf("sampled_attr_io: %d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}
