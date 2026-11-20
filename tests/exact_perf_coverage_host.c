#define _DARWIN_C_SOURCE 1
/* T-cov: real capture, writer, decoder and shared verdict; no kernel work.
 * The existing injected transport fixture supplies only daemon I/O stubs. */
#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le capture_put_le
#include "../src/merge_capture.c"
#undef put_le
#include "event_reader.h"
#include "coverage_record.h"
#include "coverage.h"
#include <dirent.h>
#include <unistd.h>

#define C(expr, why) do { if (!(expr)) { \
    fprintf(stderr, "ASSERT %s: %s (line %d)\n", which, why, __LINE__); exit(1); } } while (0)
static const char *which;
static unsigned syncs;
static bool fail_sync;
/* Only event_writer.c is compiled with fsync redirected to this observer.
 * A kill is counted only when the real GAP_OPEN path reaches the syscall. */
int coverage_test_fsync(int fd)
{
    ++syncs;
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
/* wait_event's optional psql metadata path is outside this host test. */
#include "spawn.h"
int pgwt_proc_open(struct pgwt_proc *p, char *const argv[])
{ (void)p; (void)argv; abort(); }
int pgwt_proc_close(struct pgwt_proc *p)
{ (void)p; abort(); }
struct fixture {
    struct tb_driver d; /* the injected callbacks borrow this prefix */
    struct pgwt_event_writer writer;
    struct pgwt_cov_tracker tracker;
    uint64_t base, clock, pos[4], advance_during_publish, before_publish;
    unsigned publishes;
    bool publish_fails;
    char dir[256];
};
static void gap_sink(void *ctx, const struct pgwt_capture_gap *gap)
{
    struct fixture *h = ctx;
    pgwt_cov_tracker_capture_gap(&h->tracker, &h->writer, gap);
}
static int trace_sink(void *ctx, const void *p, size_t size)
{
    struct fixture *h = ctx;
    C(size == 48, "trace payload size");
    struct pgwt_trace_event event;
    memcpy(&event, p, sizeof(event));
    return pgwt_writer_push_event(&h->writer, &event);
}
static struct fixture *coverage_setup(const uint32_t *preexisting, size_t n)
{
    struct fixture *h = calloc(1, sizeof(*h));
    C(h, "fixture allocation");
    const char *temporary=getenv("PGWT_TCOV_TMPDIR");
    int length=snprintf(h->dir,sizeof(h->dir),"%s/pgwt-tcov-XXXXXX",temporary?temporary:"/tmp");
    C(length>0 && (size_t)length<sizeof(h->dir),"temporary path bound");
    C(mkdtemp(h->dir), "temporary directory");
    struct timespec ts;
    C(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "monotonic clock");
    h->base = ((uint64_t)ts.tv_sec + 1) * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    h->clock = h->d.now = h->base + 1000;
    h->d.snapshot = readable();
    pgwt_merge_source sources[4] = {{.qualified=true}, {.qualified=true}, {.qualified=true}, {.qualified=true}};
    pgwt_merge_attachment attached = {.pid=30, .g=123, .identity_proved=true};
    pgwt_merge_config initial = {.mode=PGWT_MERGE_N, .sources=sources, .source_count=4,
        .staged_capacity=64, .payload_capacity=272, .attachment_capacity=64,
        .frontier_capacity=128, .dependency_capacity=64,
        .attachments=&attached, .attachment_count=1,
        .observation={.d=h->base, .counters={.known=true}}, .last_observed={.known=true}};
    ma_lifetime lifetime = {.pid=30, .g=123, .identity_bound=true,
        .proof=MA_PROVED, .fork_forwarded=true, .facts_known=MA_ALL_FACTS, .facts_true=MA_ALL_FACTS};
    ma_table premises = {.lifetimes=&lifetime, .lifetime_count=1};
    struct pgwt_capture_config cfg = {.cpu_count=2, .capacity=64, .ctx=h,
        .baseline=initial.observation, .trace=trace_sink, .now=tb_now,
        .test_attach_premise=true, .coverage_gap=gap_sink};
    h->d.capture = pgwt_capture_replay_create(&cfg, &initial, &premises);
    C(h->d.capture, "capture construction");
    C(pgwt_writer_init(&h->writer, h->dir, 18, 24, NULL) == 0, "writer init");
    pgwt_writer_coverage_configure(&h->writer, h->base, 0, PGWT_COVERAGE_MODE_EXACT_PERF);
    pgwt_cov_tracker_init(&h->tracker, h->base, true, NULL);
    C(pgwt_capture_coverage_begin(h->d.capture, h->base, preexisting, n, NULL) == 0, "coverage begin");
    return h;
}
static void cleanup(struct fixture *h)
{
    pgwt_capture_destroy(h->d.capture);
    free(h->tracker.capacity_gaps);
    pgwt_writer_close(&h->writer);
    pgwt_writer_destroy(&h->writer);
    DIR *dir = opendir(h->dir);
    C(dir, "open temporary directory");
    struct dirent *e;
    while ((e = readdir(dir))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
        char path[768]; snprintf(path, sizeof(path), "%s/%s", h->dir, e->d_name);
        C(unlink(path) == 0, "remove trace artifact");
    }
    closedir(dir);
    C(rmdir(h->dir) == 0, "remove temporary directory");
    free(h);
}
static void frontier(struct fixture *h, uint64_t w)
{
    for (size_t source=0; source<4; ++source) {
        ma_envelope e = {.version=1, .kind=MA_BOUNDARY, .source=source,
            .position=++h->pos[source], .w=w,
            .boundary=source<2?MA_IDLE_PERF:MA_RING_BOUNDARY};
        C(pgwt_capture_envelope(h->d.capture, &e, h->d.now, &h->d.snapshot) == 0, "boundary feed");
    }
}
static void trace(struct fixture *h, uint64_t t, bool receive_only)
{
    unsigned char bytes[48] = {0};
    tb_le(bytes, t, 8); tb_le(bytes+8, 30, 4); tb_le(bytes+24, 10, 8);
    tb_le(bytes+40, PGWT_CPU_NS_UNKNOWN, 8);
    ma_envelope e = {.version=1, .kind=MA_TRACE, .source=0,
        .position=++h->pos[0], .origin=MA_EXACT, .cookie_present=true,
        .cookie=123, .bytes=bytes, .size=sizeof(bytes)};
    size_t token=0;
    int received=pgwt_capture_received_token(h->d.capture, &e, h->d.now, &token);
    C(received >= 0, "DATA arrival");
    if (!receive_only && received==0)
        C(pgwt_capture_envelope_token(h->d.capture, &e, token, h->d.now, &h->d.snapshot) == 0, "DATA feed");
}
static struct pgwt_counter_vector coverage_counters(void *ctx)
{ (void)ctx; return (struct pgwt_counter_vector){0}; }
static uint64_t tick_clock(void *ctx) { return ((struct fixture *)ctx)->clock; }
static void certificate(void *ctx, struct pgwt_cov_tick_in *in)
{
    struct fixture *h = ctx;
    in->exact_perf = true;
    in->capture_healthy = pgwt_capture_certified_through(h->d.capture, &in->certified_through_ns);
    in->positions_known = in->capture_healthy;
    h->before_publish = in->certified_through_ns;
}
static bool publish(void *ctx)
{
    struct fixture *h = ctx;
    ++h->publishes;
    if (h->publish_fails) return false;
    int rc = pgwt_writer_publish(&h->writer);
    if (h->advance_during_publish) {
        frontier(h, h->advance_during_publish);
        h->advance_during_publish = 0;
    }
    return rc == 0;
}
static void tick(struct fixture *h, uint64_t p, uint64_t fd, uint64_t fi)
{
    h->clock = p;
    struct pgwt_cov_tick_in in = {.floors_known=true, .floor_db_ns=fd, .floor_idle_ns=fi};
    struct pgwt_counter_vector cv;
    struct pgwt_cov_tick_ops ops = {.counters=coverage_counters, .clock=tick_clock, .positions=certificate, .publish=publish};
    pgwt_cov_collect_tick(&in, &cv, h, &ops);
    C(pgwt_cov_tracker_tick(&h->tracker, &h->writer, &in) == 0, "coverage tick");
}
struct scan {
    struct pgwt_coverage_record records[128];
    struct pgwt_cov_rec facts[128];
    size_t n, data;
};
static struct scan scan(struct fixture *h)
{
    struct scan s = {0};
    if (!h->writer.fp) return s;
    struct pgwt_event_reader r;
    C(pgwt_reader_open(&r, h->writer.current_path) == 0, "read trace file");
    for (int i=0; i<r.num_blocks; ++i) {
        struct pgwt_coverage_record rec;
        uint64_t obs;
        int rc = pgwt_reader_coverage_record(&r, i, &rec, &obs);
        C(rc >= 0, "coverage decode");
        if (rc == 0) {
            C(s.n<128, "coverage record bound");
            s.records[s.n] = rec;
            int kind = rec.kind == PGWT_COVK_BOOT ? PGWT_COV_REC_FILE : rec.kind;
            s.facts[s.n++] = (struct pgwt_cov_rec){.kind=kind,
                .a=rec.kind==PGWT_COVK_BOOT?(int64_t)obs:(int64_t)rec.a,
                .b=rec.kind==PGWT_COVK_BOOT?0:(int64_t)rec.b,
                .c=rec.kind==PGWT_COVK_BOOT?1:(int64_t)rec.c};
        } else {
            struct pgwt_trace_event events[64];
            int n = pgwt_reader_decode_block(&r, i, events, 64);
            C(n >= 0, "DATA decode");
            s.data += (size_t)n;
        }
    }
    pgwt_reader_close(&r);
    return s;
}
static size_t count(const struct scan *s, unsigned kind)
{ size_t n=0; for(size_t i=0; i<s->n; ++i) n+=s->records[i].kind==kind; return n; }
static void verdict(struct fixture *h, int64_t from, int64_t to, bool complete)
{
    struct scan s = scan(h);
    struct pgwt_cov_boot boot = {.boot_start=(int64_t)h->base, .wall_offset=0,
        .closure_clean=1, .same_kernel_boot=1, .status=PGWT_COV_STATUS_MATCH,
        .last_obs=(int64_t)h->clock, .recs=s.facts, .n_recs=s.n};
    struct pgwt_cov_model model = {.boots=&boot, .n_boots=1};
    struct pgwt_cov_span spans[128];
    struct pgwt_cov_result result = {.uncovered=spans, .uncovered_cap=128};
    result.scratch_size = pgwt_cov_scratch_bytes(&model);
    result.scratch = malloc(result.scratch_size);
    C(result.scratch, "verdict scratch");
    int v = pgwt_coverage_verdict(&model, from, to, h->clock, PGWT_COV_FLOOR_DB, &result);
    if (complete) C(v==PGWT_COV_COMPLETE, "certified window complete");
    else {
        C(v==PGWT_COV_INCOMPLETE, "gap window incomplete");
        bool loss=false;
        for(size_t i=0; i<result.n_reasons; ++i) loss |= result.reasons[i]==PGWT_COV_R_CAPTURE_LOSS;
        C(loss, "gap verdict reason capture_loss");
    }
    free(result.scratch);
}
/* Compile the verbatim Linux binding with only kernel I/O replaced. This
 * catches the exact_perf selector, floor fold, startup scan and shutdown. */
static uint64_t binding_now;
static unsigned binding_reads, binding_checks;
static struct pgwt_pid_state binding_state;
struct pg_wait_tracer_bpf { struct { void *state_map; } maps; };
static int bpf_map__fd(void *map) { (void)map; return 9; }
static int bpf_map_lookup_elem(int fd, const uint32_t *pid, struct pgwt_pid_state *state)
{ C(fd==9 && *pid==77, "binding map identity"); *state=binding_state; ++binding_reads; return 0; }
static uint64_t now_ns(void) { return binding_now; }
static uint64_t cov_postmaster_start_wall_ns(pid_t pid) { (void)pid; return 0; }
static void cov_positions(void *ctx, struct pgwt_cov_tick_in *in)
{ (void)ctx; (void)in; C(false,"exact_perf must not use a ring-position fence"); }
static int binding_checkpoint(struct pgwt_daemon *d)
{ C(d->exact_perf,"exact counter checkpoint"); ++binding_checks; return 0; }
#define pgwt_merge_capture_coverage_checkpoint binding_checkpoint
#include "exact_perf_coverage_daemon.h"
#undef pgwt_merge_capture_coverage_checkpoint
static void binding(void)
{
    struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
    struct pgwt_daemon *d=calloc(1,sizeof(*d)); C(d,"binding daemon");
    struct pg_wait_tracer_bpf skel={0};
    d->start_ts=b;d->exact_perf=true;d->mode=PGWT_MODE_FULL;d->skel=&skel;
    d->merge_capture=h->d.capture;d->event_writer=&h->writer;
    d->backends.count=1;
    d->backends.entries[0]=(struct pgwt_backend){.pid=77,.is_alive=true,.wp_fd=9,.attach_ts=b+10};
    binding_state=(struct pgwt_pid_state){.last_ts=b+20,.last_event=0x06000000,.wp_live=1};
    h->d.capture->coverage_begun=false; /* startup config precedes the boot */
    h->d.capture->config.ctx=d;h->d.capture->config.now=NULL;
    binding_now=b+100;
    pgwt_coverage_start(d,NULL);
    C(d->coverage.watermarks,"exact_perf startup enables watermarks");
    struct scan s=scan(h);
    C(count(&s,PGWT_COVK_GAP_OPEN)==1,"startup discovers preexisting backend");
    frontier(h,b+200);
    pgwt_coverage_tick(d);
    binding_now=b+210;pgwt_coverage_tick(d);
    C(d->coverage.w_ns==b+100 && d->coverage.fd_ns==b+100 && d->coverage.fi_ns==b+20,
      "daemon exact_perf uses pending floor fold and frontier");
    C(binding_checks==2 && binding_reads==3,"daemon reads floors and checkpoints every tick");
    binding_now=b+220;pgwt_coverage_shutdown(d);
    s=scan(h);C(count(&s,PGWT_COVK_SEAL)==1,"exact_perf shutdown seals");
    free(d);cleanup(h);
}
static void progress(void)
{
    struct fixture *h=coverage_setup(NULL, 0); uint64_t b=h->base;
    trace(h,b+90,false); frontier(h,b+100);
    tick(h,b+120,b+20,b+10);
    frontier(h,b+200);
    tick(h,b+220,b+210,b+205);
    C(h->tracker.w_ns==b+120 && h->tracker.fd_ns==b+20 && h->tracker.fi_ns==b+10,
      "floors stay paired with their P");
    struct scan s=scan(h);
    C(s.data==1 && h->publishes==2, "successful publish puts certified DATA in trace");
    C(h->tracker.w_ns<=h->before_publish, "W bounded by preceding publish certificate");
    tick(h,b+230,b+225,b+225);
    s=scan(h);
    C(count(&s,PGWT_COVK_WATERMARK)==3 && h->tracker.w_ns==b+120 &&
      h->tracker.fd_ns==b+20, "stationary frontier heartbeats repeat triple");
    verdict(h,b+1,b+19,true);
    cleanup(h);
}
static void publish_order(void)
{
    struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
    frontier(h,b+100); tick(h,b+120,b+120,b+120);
    h->advance_during_publish=b+200;
    tick(h,b+220,b+220,b+220);
    C(h->tracker.w_ns==b, "cursor read before publish cannot certify concurrent advance");
    tick(h,b+230,b+230,b+230);
    C(h->tracker.w_ns==b+120, "next publish may certify older pending point");
    cleanup(h);
}
static void failure(void)
{
    struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
    frontier(h,b+100); tick(h,b+120,b+120,b+120);
    frontier(h,b+200);
    struct scan before=scan(h);
    h->publish_fails=true;
    tick(h,b+220,b+220,b+220);
    struct scan after=scan(h);
    C(count(&after,PGWT_COVK_WATERMARK)==count(&before,PGWT_COVK_WATERMARK),
      "failed publish writes no WATERMARK");
    h->publish_fails=false;
    C(pgwt_capture_fault(h->d.capture,PGWT_MERGE_UNKNOWN_EVIDENCE,h->d.now,&h->d.snapshot)==0,"suspend");
    unsigned published=h->publishes;
    before=scan(h); tick(h,b+230,b+230,b+230); after=scan(h);
    C(h->publishes==published, "suspended capture does not publish");
    C(count(&after,PGWT_COVK_WATERMARK)==count(&before,PGWT_COVK_WATERMARK),
      "suspended capture writes no WATERMARK");
    cleanup(h);
}
static void reasons(void)
{
    for(unsigned reason=0; reason<PGWT_MERGE_REASON_COUNT; ++reason) {
        struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
        frontier(h,b+100); tick(h,b+50,b+50,b+50); tick(h,b+110,b+110,b+110);
        unsigned sync_before=syncs;
        /* Internal-breach faults return -EIO; the VOID must still persist. */
        (void)pgwt_capture_fault(h->d.capture,(pgwt_merge_reason)reason,h->d.now,&h->d.snapshot);
        struct scan s=scan(h);
        C(count(&s,PGWT_COVK_GAP_OPEN)==1, "every VOID opens GAP");
        C(syncs==sync_before+1, "every GAP_OPEN fsyncs");
        struct pgwt_coverage_record gap={0};
        for(size_t i=0;i<s.n;++i) if(s.records[i].kind==PGWT_COVK_GAP_OPEN) gap=s.records[i];
        unsigned expected=reason==PGWT_MERGE_COUNTER_CHANGE || reason==PGWT_MERGE_PERF_LOST ? PGWT_COVR_DROP :
            reason==PGWT_MERGE_COUNTER_UNKNOWN ? PGWT_COVR_COUNTERS_UNKNOWN : PGWT_COVR_PAUSE;
        C(gap.reason==expected && gap.a==b+100, "VOID reason and exclusive start");
        tick(h,b+120,b+120,b+120); s=scan(h);
        C(count(&s,PGWT_COVK_WATERMARK)==2 && count(&s,PGWT_COVK_GAP_CLOSE)==0,
          "permanent VOID never closes or writes another W");
        verdict(h,b+100,b+101,false);
        cleanup(h);
    }
    struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
    (void)pgwt_capture_fault(h->d.capture,PGWT_MERGE_PERF_LOST,h->d.now,&h->d.snapshot);
    struct scan s=scan(h);
    C(count(&s,PGWT_COVK_GAP_OPEN)==1 && s.records[1].a==b, "no frontier gap begins at boot_start");
    cleanup(h);
}
static void rearm(void)
{
    struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
    frontier(h,b+100); tick(h,b+50,b+50,b+50); tick(h,b+110,b+110,b+110);
    pgwt_allocation_failure allocation={.pool=PGWT_POOL_CAPTURE_ARRIVAL};
    C(pgwt_capture_allocation_fault(h->d.capture,allocation,h->d.now,&h->d.snapshot)==0,"capacity VOID");
    pgwt_capture_discard_backlog(h->d.capture);
    /* Production gate stays closed, even with an otherwise valid snapshot. */
    h->d.capture->config.test_attach_premise=false;
    C(pgwt_capture_rearm(h->d.capture,b+200,&h->d.snapshot)==0,"production rearm remains refused");
    h->d.capture->config.test_attach_premise=true;
    h->d.now=b+2000;
    C(pgwt_capture_rearm(h->d.capture,b+200,&h->d.snapshot)==1,"test REARM");
    uint64_t at=h->d.capture->rearm_at;
    struct scan s=scan(h);
    C(count(&s,PGWT_COVK_GAP_CLOSE)==0, "REARM must not close capacity gap");
    frontier(h,at); s=scan(h);
    C(count(&s,PGWT_COVK_GAP_CLOSE)==0, "frontier below rearm_at must not close gap");
    frontier(h,at+1); s=scan(h);
    C(count(&s,PGWT_COVK_GAP_CLOSE)==1, "first qualifying FRONTIER closes capacity gap");
    C(s.records[s.n-1].kind==PGWT_COVK_GAP_CLOSE && s.records[s.n-1].a==at,"GAP_CLOSE at proof completion");
    tick(h,at+10,at+10,at+10); frontier(h,at+30); tick(h,at+40,at+40,at+40);
    verdict(h,b+100,at,false); verdict(h,at+1,at+9,true);
    cleanup(h);
}
static void late(void)
{
    for(unsigned preproof=0;preproof<2;++preproof) {
        struct fixture *h=coverage_setup(NULL,0); uint64_t b=h->base;
        frontier(h,b+200); tick(h,b+120,b+120,b+120); tick(h,b+220,b+220,b+220);
        if(preproof) {
            C(pgwt_capture_allocation_fault(h->d.capture,
              (pgwt_allocation_failure){.pool=PGWT_POOL_TRANSPORT_QUEUE},h->d.now,&h->d.snapshot)==0,"preproof capacity VOID");
            pgwt_capture_discard_backlog(h->d.capture);h->d.now=b+2000;
            C(pgwt_capture_rearm(h->d.capture,b+1000,&h->d.snapshot)==1,"preproof REARM");
        }
        trace(h,b+90,false);
        C(pgwt_capture_report(h->d.capture).epoch_invalid,"late DATA invalidates epoch");
        /* Received while permanently suspended; transport will not enqueue it. */
        trace(h,b+40,true);
        struct scan s=scan(h); size_t seen=0;
        for(size_t i=0;i<s.n;++i) if(s.records[i].kind==PGWT_COVK_GAP_OPEN &&
            (!preproof || s.records[i].reason!=PGWT_COVR_PAUSE)) {
            C(s.records[i].reason==PGWT_COVR_REVOKED,"each late record revokes");
            C(s.records[i].a==b+(seen?40:90),"second earlier DATA extends revocation"); ++seen;
        }
        C(seen==2,"both late DATA gaps persisted");
        verdict(h,b+40,b+41,false); cleanup(h);
    }
}
static void preexisting(void)
{
    uint32_t pids[]={11,12}; struct fixture *h=coverage_setup(pids,2); uint64_t b=h->base;
    struct scan s=scan(h);
    C(count(&s,PGWT_COVK_GAP_OPEN)==1 && s.records[1].a==b && s.records[1].reason==PGWT_COVR_PAUSE,
      "preexisting gap begins at boot_start");
    for(unsigned i=0;i<2;++i) {
        unsigned char exit_bytes[24]={0}; tb_le(exit_bytes,PGWT_LIFECYCLE_EXIT,4);tb_le(exit_bytes+4,pids[i],4);
        /* Receive the later EXIT first: close at the last actual exit. */
        tb_le(exit_bytes+16,b+(i?180:190),8);
        ma_envelope e={.version=1,.kind=MA_LIFECYCLE,.bytes=exit_bytes,.size=24,.source=3,.position=++h->pos[3]};
        C(pgwt_capture_received(h->d.capture,&e,h->d.now)==1,"uncovered EXIT counted and dropped");
        s=scan(h); C(count(&s,PGWT_COVK_GAP_CLOSE)==i,"only last backend closes gap");
    }
    C(s.records[s.n-1].a==b+190,"preexisting closes at latest exit time");
    frontier(h,b+300); tick(h,b+250,b+250,b+250); frontier(h,b+400); tick(h,b+410,b+410,b+410);
    verdict(h,b+1,b+10,false); verdict(h,b+200,b+240,true); cleanup(h);
    h=coverage_setup(NULL,0);b=h->base;frontier(h,b+300);tick(h,b+100,b+100,b+100);tick(h,b+310,b+310,b+310);
    s=scan(h);C(count(&s,PGWT_COVK_GAP_OPEN)==0,"no preexisting backend opens no gap");cleanup(h);
}
static void sync_failure(void)
{
    struct fixture *h=coverage_setup(NULL,0);uint64_t b=h->base;
    frontier(h,b+100);tick(h,b+50,b+50,b+50);tick(h,b+110,b+110,b+110);
    fail_sync=true;
    (void)pgwt_capture_allocation_fault(h->d.capture,(pgwt_allocation_failure){.pool=PGWT_POOL_CORE_STAGING},h->d.now,&h->d.snapshot);
    fail_sync=false;
    C(h->tracker.gap_failed,"failed GAP_OPEN permanently disables watermarks");
    pgwt_capture_discard_backlog(h->d.capture);h->d.now=b+2000;
    C(pgwt_capture_rearm(h->d.capture,b+200,&h->d.snapshot)==1,"test rearm after disk failure");
    frontier(h,b+2100);tick(h,b+2100,b+2100,b+2100);
    struct scan s=scan(h);C(count(&s,PGWT_COVK_WATERMARK)==2,"no W after failed GAP fsync even after rearm");cleanup(h);
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"reader_export")) {
        which="reader_export";
        struct fixture *h=coverage_setup(NULL,0);uint64_t b=h->base;
        trace(h,b+90,false);frontier(h,b+200);
        tick(h,b+120,b+120,b+120);tick(h,b+220,b+220,b+220);
        (void)pgwt_capture_fault(h->d.capture,PGWT_MERGE_PERF_LOST,h->d.now,&h->d.snapshot);
        /* Process exit without writer_close models a crash; Python removes
         * this exported temporary directory after testing the live reader. */
        printf("EXPORTED %s %llu\n",h->dir,(unsigned long long)b);
        pgwt_capture_destroy(h->d.capture);
        free(h->tracker.capacity_gaps);
        if(h->writer.fp) { fclose(h->writer.fp); h->writer.fp=NULL; }
        pgwt_writer_destroy(&h->writer);free(h);
        return 0;
    }
    const char *names[]={"progress","publish_order","failure","reasons","rearm","late","preexisting","sync_failure","binding"};
    void (*run[])(void)={progress,publish_order,failure,reasons,rearm,late,preexisting,sync_failure,binding};
    unsigned ran=0;
    for(size_t i=0;i<sizeof(run)/sizeof(run[0]);++i) {
        if(argc>1 && strcmp(argv[1],names[i])) continue;
        which=names[i]; printf("REACHED T-cov %s\n",which);fflush(stdout);
        run[i]();printf("T-cov %s: PASS\n",which);++ran;
    }
    C(ran>0,"named case reached");return 0;
}
