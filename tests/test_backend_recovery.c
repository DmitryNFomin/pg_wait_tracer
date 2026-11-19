/* Deterministic /proc fixture around unchanged recovery and FORK handlers.
 * No PostgreSQL, kernel watchpoints or wall-clock thresholds. */
#define _POSIX_C_SOURCE 200809L
#include "daemon.h"
#include "backend_init.h"
#include "capture_counters.h"
#include "discovery.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x, message) do { if (!(x)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); exit(1); } } while (0)
struct pg_wait_tracer_bpf { struct { void *on_bootstrap; } progs; };
static struct pgwt_daemon d;
static unsigned decoys, enumerated, walks, stat_opens, children_opens, children_reads;
static unsigned attempts, init_calls, enrollments;
static int unavailable, attach_fails, init_no_attach, interrupted, bootstrap_shared = 1;
static const char *list = "42 43 44 45 46 47 ";
static size_t list_offset, list_length;
static uint64_t now_ns(void) { return 100; }
uint64_t pgwt_debug_monotonic_ns(void) { return 100; }
void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t ns)
{ (void)op; (void)pid; (void)ns; }
void pgwt_pgbs_reader_reset(struct pgwt_pgbs_reader *r) { memset(r, 0, sizeof(*r)); }
bool pgwt_pgbs_pid_is_original(pid_t pid, uint64_t ts) { return pid == 42 && ts == 1; }
int pgwt_sampler_enroll_backend(struct pgwt_backend *be, uint64_t ns)
{ (void)ns; if (!be->sampled_reader.pid) ++enrollments; be->sampled_reader.pid=be->pid; be->sampled_reader.start_time=1; return 0; }
bool pgwt_pgbs_exclusion_contains(const struct pgwt_pgbs_exclusion_set *s, pid_t pid)
{ (void)s; return pid == 43; }
static int lifecycle_target(struct pgwt_daemon *ctx, pid_t pid)
{ CHECK(ctx == &d && pid == 42, "live postmaster child"); return 1; }
int pgwt_handle_exit(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; CHECK(0, "unexpected exit"); return 0; }
static void remember_enrollment_refusal(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; (void)pid; CHECK(0, "unexpected allocation failure"); }
static int bpf_program__fd(void *prog) { (void)prog; return 7; }
static int open_watchpoint_target(pid_t pid) { (void)pid; return -1; }
static void record_watchpoint_failure(struct pgwt_daemon *ctx, int fd, pid_t pid,
                                     const char *site, int error, int resolve_error)
{ (void)ctx; (void)fd; (void)pid; (void)site; (void)error; (void)resolve_error; CHECK(0, "unexpected bootstrap failure"); }
int pgwt_open_watchpoint(pid_t pid, uint64_t addr, int prog)
{ CHECK(pid == 42 && addr == 123 && prog == 7, "bootstrap attach arguments"); ++attempts; return 99; }
uint64_t pgwt_resolve_backend_wait_addr(struct pgwt_daemon *ctx, pid_t pid)
{ (void)ctx; return pid == 46 ? 0 : 1234; }
int pgwt_addr_is_shared(pid_t pid, uint64_t addr)
{ CHECK(addr == 1234, "resolved address"); return pid == 47 ? 0 : bootstrap_shared; }
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *ctx, uint32_t slot)
{ (void)ctx; (void)slot; return (struct pgwt_counter_value){0, true}; }
static struct pgwt_backend *alloc_backend(struct pgwt_backend_table *, pid_t, bool);
/* Model the actual INIT failure shape: recovery allocates/enrolls an entry,
 * then validation/attachment fails. A later FORK must still attempt hardware. */
int pgwt_handle_init(struct pgwt_daemon *ctx, pid_t pid, uint64_t addr, uint64_t ts)
{
    (void)ts; CHECK(ctx == &d && pid == 42 && addr == 1234, "eligible child only");
    ++init_calls;
    struct pgwt_backend *be=pgwt_find_backend(&ctx->backends, pid);
    if (!be) {
        be=alloc_backend(&ctx->backends, pid, false);
        if (be) be->init_diag_cookie=ctx->init_diagnostics.receipt_cookie;
    }
    CHECK(be, "INIT allocates recovery entry");
    pgwt_sampler_enroll_backend(be, now_ns());
    if (attach_fails) return -1;
    if (init_no_attach) return 0;
    pgwt_pending_init_remove(&ctx->backends, be);
    be->bootstrap_fd=-1; be->wp_fd=101; be->init_attached=true;
    if (ctx->attach_timeline) {
        pgwt_timeline_phase(ctx->attach_timeline, pid, be->init_diag_cookie,
                            PGWT_TL_WAIT_OPEN, true);
        pgwt_timeline_phase(ctx->attach_timeline, pid, be->init_diag_cookie,
                            PGWT_TL_WAIT_ENABLE, true);
    }
    return 0;
}

static int proc_open(const char *path, int flags)
{
    CHECK(!strcmp(path, "/proc/7/task/7/children"), "children pathname");
    CHECK(flags == (O_RDONLY | O_CLOEXEC), "children flags");
    ++children_opens;
    list_offset=0;
    if (unavailable == 1) { errno=ENOENT; return -1; }
    return 55;
}
static ssize_t proc_read(int fd, void *out, size_t size)
{
    CHECK(fd == 55 && size > 0, "children read arguments"); ++children_reads;
    if (size > 4096) size=4096; /* seq_file page limit, including fill control */
    if (unavailable == 2 || (unavailable == 4 && list_offset >= 4096)) {
        errno=EIO; return -1;
    }
    if (interrupted && (children_reads == 1 || children_reads == 3)) {
        errno=EINTR; return -1;
    }
    if (unavailable == 3) { memset(out, '1', size); return (ssize_t)size; }
    size_t length=list_length ? list_length : strlen(list);
    size_t remaining=length-list_offset;
    if (size > remaining) size=remaining;
    memcpy(out, list+list_offset, size); list_offset+=size;
    return (ssize_t)size;
}
static int proc_close(int fd) { CHECK(fd == 55, "children fd closed"); return 0; }
static FILE *proc_fopen(const char *path, const char *mode)
{
    int pid=0; CHECK(sscanf(path, "/proc/%d/stat", &pid) == 1 && !strcmp(mode,"r"), "stat path");
    ++stat_opens;
    FILE *fp=tmpfile(); CHECK(fp, "stat fixture file");
    if (pid == 45) fputs("malformed stat\n", fp);
    else fprintf(fp, "%d (worker (with spaces)) S %d 0 0\n", pid,
                 pid == 44 || pid >= 1000 ? 8 : 7);
    rewind(fp); return fp;
}
static DIR *proc_opendir(const char *path)
{ CHECK(!strcmp(path,"/proc"), "fallback directory"); ++walks; enumerated=0; return (DIR *)&walks; }
static struct dirent *proc_readdir(DIR *dir)
{
    CHECK(dir == (DIR *)&walks, "fallback handle");
    static struct dirent ent;
    if (enumerated >= decoys + 6) return NULL;
    unsigned pid=enumerated < 6 ? 42 + enumerated : 1000 + enumerated - 6;
    ++enumerated; snprintf(ent.d_name, sizeof(ent.d_name), "%u", pid); return &ent;
}
static int proc_closedir(DIR *dir) { CHECK(dir == (DIR *)&walks,"close fallback"); return 0; }
#define open proc_open
#define read proc_read
#define close proc_close
#define fopen proc_fopen
#define opendir proc_opendir
#define readdir proc_readdir
#define closedir proc_closedir
#include PGWT_RECOVERY_SOURCE
#undef open
#undef read
#undef close
#undef fopen
#undef opendir
#undef readdir
#undef closedir

static void reset(void)
{
    static struct pg_wait_tracer_bpf skel;
    memset(&d,0,sizeof(d)); d.skel=&skel; d.postmaster_pid=7; d.my_wait_ptr_addr=123;
    pgwt_backend_init(&d.backends);
    decoys=512; enumerated=walks=stat_opens=children_opens=children_reads=0;
    attempts=init_calls=enrollments=0;
    unavailable=attach_fails=init_no_attach=interrupted=0; bootstrap_shared=1;
    list_offset=list_length=0;
    list="42 43 44 45 46 47 ";
}
int main(void)
{
    puts("REACHED backend recovery"); fflush(stdout);
    reset();
    CHECK(pgwt_recover_unattached_backends(&d) == 1, "straddler recovered");
    CHECK(init_calls == 1 && d.backends.count == 1, "eligibility: parent, exclusion, stat, resolve, shared");
    CHECK(stat_opens <= 6 + 1 && children_opens == 1 && !walks,
          "stat opens bounded by children plus constant despite 512 decoys");
    unsigned first_stat_opens=stat_opens;
    CHECK(pgwt_recover_unattached_backends(&d) == 0 && init_calls == 1, "attached recovery idempotent");
    printf("children=6 decoys=%u first-pass stat opens=%u; read through EOF, no walk: PASS\n", decoys, first_stat_opens);
    for (int failure=1; failure<=3; ++failure) {
        reset(); unavailable=failure;
        CHECK(pgwt_recover_unattached_backends(&d) == 1 && init_calls == 1 && walks == 1,
              "fallback finds child after open/read/truncation failure");
    }
    puts("children open/read/truncation fallback and eligibility: PASS");
    /* 600 seven-digit PIDs precede the only eligible child, past page one. */
    char paged[600 * 8 + 4];
    for (unsigned i=0; i<600; ++i)
        snprintf(paged+8*i,sizeof(paged)-8*i,"%u ",1000000+i);
    memcpy(paged+4800,"42 ",4);
    for (int interrupt=0; interrupt<=1; ++interrupt) {
        reset(); list=paged; interrupted=interrupt;
        puts("REACHED child after first seq_file page"); fflush(stdout);
        CHECK(pgwt_recover_unattached_backends(&d) == 1 && init_calls == 1 && !walks,
              "child past first page recovered without fallback");
        CHECK(children_reads == (interrupt ? 5U : 3U), "reads continue through EINTR and EOF");
    }
    reset(); list=paged; unavailable=4;
    CHECK(pgwt_recover_unattached_backends(&d) == 1 && walks == 1,
          "read failure after first page falls back");
    puts("page-limited reads, late child, EINTR and late read failure: PASS");
    const char *malformed[]={"oops", "42x ", "0 ", "-42 ", "+42 ", "9999999999999999999999999 ", "42 oops"};
    for (unsigned i=0; i<sizeof(malformed)/sizeof(malformed[0]); ++i) {
        reset(); list=malformed[i];
        CHECK(pgwt_recover_unattached_backends(&d) == 1 && walks == 1 && init_calls == 1,
              "malformed children list falls back without duplicate attach");
    }
    reset(); list="1000000 \0 42 "; list_length=13;
    CHECK(pgwt_recover_unattached_backends(&d) == 1 && walks == 1,
          "embedded NUL falls back");
    puts("malformed children lists fall back: PASS");

    for (int failure=0; failure<=2; ++failure) {
        reset();
        d.attach_timeline=calloc(1,sizeof(*d.attach_timeline)); CHECK(d.attach_timeline,"timeline allocation");
        struct pgwt_lifecycle_event fork={.type=PGWT_LIFECYCLE_FORK,.pid=42,.timestamp=10};
        pgwt_timeline_receive(d.attach_timeline,&fork,7);
        struct pgwt_timeline_row *row=pgwt_timeline_find(d.attach_timeline,42,7,false);
        CHECK(row,"FORK row exists before recovery");
        /* Earlier pending/completed incarnations must not receive credit. */
        struct pgwt_timeline_row *old=pgwt_timeline_find(d.attach_timeline,42,6,true);
        CHECK(old,"old row"); old->fork_ns=5;
        struct pgwt_timeline_row *done=pgwt_timeline_find(d.attach_timeline,42,5,true);
        CHECK(done,"completed row"); done->fork_ns=20; done->slot=2; --d.attach_timeline->pending;
        d.init_diagnostics.receipt_cookie=99;
        attach_fails=failure==1; init_no_attach=failure==2;
        pgwt_recover_unattached_backends(&d);
        puts("REACHED recovery attach with existing FORK cookie"); fflush(stdout);
        CHECK(row->attached_by_recovery == (failure==0), "recovery credits FORK lifetime only on real attach");
        CHECK(!old->attached_by_recovery && !done->attached_by_recovery,
              "recovery leaves earlier incarnations alone");
        CHECK(!memcmp(&d.counters,&(struct pgwt_counters){0},sizeof(d.counters)),
              "timeline recovery leaves counters unchanged");
        struct pgwt_lifecycle_event exit_record={.type=PGWT_LIFECYCLE_EXIT,.pid=42,
            .timestamp=200,.exit_flags=PGWT_EXIT_DISPOSITION};
        pgwt_timeline_receive(d.attach_timeline,&exit_record,7);
        FILE *sink=tmpfile(); CHECK(sink,"timeline output");
        pgwt_timeline_finish(sink,d.attach_timeline,42,7);
        rewind(sink); char json[2048]; size_t bytes=fread(json,1,sizeof(json)-1,sink); json[bytes]=0;
        CHECK(strstr(json,failure ? "\"outcome\":\"never_fired\"" : "\"outcome\":\"attached\""),
              "FORK row outcome reflects recovery attachment");
        CHECK(strstr(json,"\"cookie\":7") && strstr(json,"\"lost\":false"), "lifetime identity and loss unchanged");
        fclose(sink); free(d.attach_timeline->samples); free(d.attach_timeline);
    }
    puts("recovery credits FORK row outcome=attached; failures and counters preserved: PASS");
    reset(); attach_fails=1;
    CHECK(pgwt_recover_unattached_backends(&d) == 0 && init_calls == 1 && d.backends.count == 1,
          "recovery creates unattached entry");
    attach_fails=0;
    CHECK(pgwt_handle_fork(&d,42) == 0 && attempts == 1 && init_calls == 2,
          "FORK after recovery must attempt attach");
    CHECK(d.backends.count == 1 && enrollments == 1 && d.backends.entries[0].wp_fd == 101,
          "one backend and one enrollment");
    CHECK(pgwt_handle_fork(&d,42) == 0 && attempts == 1 && init_calls == 2,
          "duplicate FORK leaves live watchpoint alone");
    reset(); attach_fails=1; CHECK(pgwt_recover_unattached_backends(&d) == 0,"unattached fixture");
    bootstrap_shared=0;
    CHECK(pgwt_handle_fork(&d,42) == 0 && attempts == 1 && d.backends.entries[0].bootstrap_fd == 99,
          "bootstrap starts on existing unattached entry");
    CHECK(pgwt_handle_fork(&d,42) == 0 && attempts == 1 && d.backends.pending_init_count == 1,
          "bootstrap in progress stays idempotent");
    puts("recovery then FORK: direct/bootstrap attach, duplicate watchpoints/counts: PASS");
    reset(); list="";
    CHECK(pgwt_recover_unattached_backends(&d) == 0 && !walks && !stat_opens,"empty children list");
    reset(); d.mode=PGWT_MODE_SAMPLED;
    CHECK(pgwt_recover_unattached_backends(&d) == 0 && !children_opens && !walks,"sampled mode guard");
    puts("backend recovery: PASS"); return 0;
}
