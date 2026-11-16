/* Portable workload/queue model for the REAL probe service loop, instrument
 * routine, drain and statistics functions inserted by the registered host
 * driver. No kernel, BPF load, daemon or product numerical answers. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include "pg_wait_tracer.h"
#include "pgwt_transport_wire.h"
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", label); return 1; } } while (0)
/* PROBE_STATS */
static void probe_note_work(struct probe_work_stat *work);
struct bpf_link { int unused; };
/* PROBE_BACKENDS */
struct probe_stamp { u32 cpu, round; u64 stamp; };
struct fake_skel {
    struct { void *on_bootstrap, *on_report_activity, *on_executor_start, *on_report_query_id; } progs;
    struct { int pg13_query_attr; } *rodata;
    struct { void *ringbuf_site_drops, *transport_discards; } maps;
};
struct ring { u64 consumer, producer; };
struct probe {
    struct probe_stats stats;
    struct bpf_link *activity, *query;
    struct probe_backend backends[MAX_BACKENDS];
    struct fake_skel *skel; void *rings, *perf;
    u64 ptr, activity_offset, query_offset;
    const char *exe; int failed, possible; bool loaded;
    int overflow, drain_error, retire, drains;
    bool self, stopping, spinning, init_mutant;
    struct probe_identity *identities; size_t identities_cap;
    struct probe_pid_cookie *pid_lifetimes;
    u64 *last_seq, *delivered, *discard_scratch, *skipped, *acks;
    u64 records[5], boundary_w[3]; u32 boundary_round[3];
    struct probe_stamp *helper_stamps; size_t helper_stamps_len, helper_stamps_cap;
    u64 forks, reuses, init_records, exited_before_attach;
    bool emit_exits, churn;
    unsigned churn_emitted;
};
static struct ring lifecycle;
static struct probe *active;
static bool in_callback, callback_teardown;
static int closes, destroys, global_attaches, pid_attaches;
static int fail_uprobe_at, uprobe_calls;
static void *query_program;
static u64 fake_now;
static u64 probe_now(void) { return fake_now; }
static int close(int fd)
{
    (void)fd; callback_teardown |= in_callback; ++closes; fake_now+=1000;
    return 0;
}
static void bpf_link__destroy(struct bpf_link *link)
{
    if (!link) return;
    callback_teardown |= in_callback; ++destroys; fake_now+=37500000;
}
static int probe_l(void *ctx, void *data, size_t size);
static void emit_lifecycle(struct probe *p, u32 type, u32 pid, u64 cookie)
{
    struct { struct pgwt_wire_hdr hdr; struct pgwt_lifecycle_event payload; } wire={
        .hdr={.version=PGWT_WIRE_VERSION, .kind=PGWT_WIRE_LIFECYCLE,
            .payload_len=sizeof(wire.payload), .cookie=cookie,
            .seq=p->last_seq[PGWT_WIRE_DEST_L*p->possible]+1},
        .payload={.type=type, .pid=pid, .timestamp=type==PGWT_LIFECYCLE_FORK ? 100 : 200}};
    probe_l(p, &wire, sizeof(wire));
}
static void emit_exit(struct probe *p, int slot)
{
    struct probe_backend *b=&p->backends[slot];
    emit_lifecycle(p, PGWT_LIFECYCLE_EXIT, b->pid, b->cookie);
}
static int ring_buffer__consume(void *rings)
{
    (void)rings;
    if (active->drain_error) return -1;
    ++active->drains;
    fake_now+=active->drains%2 ? 9000 : 25000;
    in_callback=true;
    if (active->emit_exits) {
        /* No occupancy before consume; producers advance during it. Every
         * callback must sample, so the second EXIT sets the maximum. */
        lifecycle.producer+=12000;
        emit_exit(active, 0);
        lifecycle.producer+=32000;
        emit_exit(active, 1);
        active->emit_exits=false;
    }
    if (active->churn) {
        /* Backends arrive every millisecond even while the reader is
         * tearing down older ones. Link destruction advances fake time by
         * 37.5 ms each (two links = the measured ~75 ms/backend). */
        while (active->churn_emitted < 4096 &&
               fake_now >= (active->churn_emitted+1)*1000000ULL) {
            u64 cookie=++active->churn_emitted+32;
            u32 pid=(u32)cookie+10000;
            emit_lifecycle(active, PGWT_LIFECYCLE_FORK, pid, cookie);
            emit_lifecycle(active, PGWT_LIFECYCLE_EXIT, pid, cookie);
        }
    }
    in_callback=false;
    lifecycle.consumer = lifecycle.producer;
    if (active->retire) {
        /* EXIT and a new FORK reused this slot while consuming L. */
        active->backends[0] = (struct probe_backend){.pid=999, .pending=true, .boot=-1, .wait=-1};
        active->retire = 0;
    }
    return 1;
}
static struct ring *ring_buffer__ring(void *rings, int index)
{ (void)rings; return index == 1 ? &lifecycle : NULL; }
static u64 ring__consumer_pos(struct ring *r) { return r->consumer; }
static u64 ring__producer_pos(struct ring *r) { return r->producer; }
static int perf_buffer__consume(void *perf) { (void)perf; return 0; }
static void probe_fail(struct probe *p, const char *why) { (void)why; p->failed=1; }
/* Each attach lets a producer add records. Draining between backends fits
 * the finite queue; deferring all drains until the end loses data. */
static void traffic(struct probe *p, u64 bytes)
{
    lifecycle.producer += bytes;
    if (lifecycle.producer - lifecycle.consumer >= 65536) p->overflow=1;
}
static int probe_watchpoint(struct probe *p, u32 pid, u64 addr, int program, bool enable)
{
    (void)pid; (void)addr; (void)program; (void)enable;
    probe_note_work(&p->stats.attaches); traffic(p, 20000); return 7;
}
static int bpf_program__fd(void *program) { (void)program; return 1; }
static struct bpf_link *bpf_program__attach_uprobe(void *program, bool ret, pid_t pid, const char *exe, size_t offset)
{
    (void)ret; (void)exe;
    ++uprobe_calls;
    if (uprobe_calls == fail_uprobe_at) return (struct bpf_link *)-5;
    if (pid == -1) ++global_attaches; else ++pid_attaches;
    if (offset == active->query_offset) query_program=program;
    traffic(active, 20000); return (struct bpf_link *)1;
}
static long libbpf_get_error(void *link) { return (intptr_t)link == -5 ? -5 : 0; }
static void probe_attach_error(struct probe *p, struct probe_backend *b, const char *step, int error)
{ (void)b; (void)step; (void)error; p->failed=1; }
static void probe_try_attach(struct probe *p, struct probe_backend *b)
{
    if (!b->pid || b->pending || b->closing) return;
    /* Reading already instrumented backends must also be interleaved with
     * drains, rather than escaping a pending-only attach budget. */
    for (int i=0; i<4; ++i) { probe_note_work(&p->stats.proc_reads); traffic(p, 1000); }
    if (b->wait < 0) b->wait=probe_watchpoint(p, b->pid, 0, 0, false);
}
static int bpf_map__fd(void *map) { (void)map; return 1; }
static int counter_error;
static int bpf_map_lookup_elem(int fd, const u32 *slot, u64 *values)
{
    (void)fd;
    if (counter_error) return -1;
    values[0]=*slot; values[1]=*slot+1; return 0;
}
/* PROBE_FUNCTIONS */
static void setup(struct probe *p)
{
    static struct fake_skel skel;
    static typeof(*skel.rodata) rodata;
    static struct probe_identity identities[8192];
    static struct probe_pid_cookie lifetimes[1U<<20];
    static u64 seq[6], delivered[6], skipped[6], scratch[2], acks[2];
    memset(identities, 0, sizeof(identities)); memset(lifetimes, 0, sizeof(lifetimes));
    memset(seq, 0, sizeof(seq)); memset(delivered, 0, sizeof(delivered));
    memset(skipped, 0, sizeof(skipped)); memset(scratch, 0, sizeof(scratch));
    memset(acks, 0, sizeof(acks));
    memset(&skel, 0, sizeof(skel));
    memset(&rodata, 0, sizeof(rodata));
    skel.rodata=&rodata;
    skel.progs.on_report_activity=(void *)1;
    skel.progs.on_report_query_id=(void *)2;
    skel.progs.on_executor_start=(void *)3;
    *p=(struct probe){.skel=&skel, .activity_offset=1, .query_offset=2, .possible=2, .loaded=true,
        .identities=identities, .identities_cap=8192, .pid_lifetimes=lifetimes,
        .last_seq=seq, .delivered=delivered, .skipped=skipped, .discard_scratch=scratch, .acks=acks};
    lifecycle=(struct ring){0}; active=p;
    in_callback=callback_teardown=false; closes=destroys=global_attaches=pid_attaches=0; fake_now=0;
    uprobe_calls=fail_uprobe_at=0; query_program=NULL;
    p->stats.registry_max_used=16;
    for (int i=0; i<16; ++i) {
        p->backends[i]=(struct probe_backend){.pid=(u32)i+1, .cookie=(u64)i+1,
            .pending=true, .boot=-1, .wait=-1};
        identities[i+1]=(struct probe_identity){.pid=(u32)i+1, .fork_seen=true, .fork_ts=100};
    }
}
static int churn_registry(void)
{
    struct probe p; setup(&p);
    CHECK(probe_attach_uprobes(&p)==0, "churn global uprobe setup");
    probe_service_backends(&p);
    /* Instrumented initial cohort exits together, then thousands of short
     * lifetimes exit between drains. Each keeps its slot until fd teardown.
     * The old per-backend links make 16 exits cost 1.2 s and fill 1024 slots. */
    in_callback=true;
    for (int i=0; i<16; ++i) emit_exit(&p, i);
    in_callback=false;
    CHECK(!callback_teardown, "backend teardown runs outside lifecycle callback");
    CHECK(p.backends[0].pid==1 && p.backends[0].closing && p.backends[0].wait>=0,
          "closing slot reserved until deferred teardown");
    fake_now=0; p.churn=true;
    for (int i=0; i<4096; ++i) {
        fake_now+=1000000;
        probe_drain(&p);
        CHECK(!p.failed, "churn registry accepts backends without exhaustion");
    }
    p.churn=false;
    while (probe_teardown_one(&p)) {}
    CHECK(!callback_teardown, "backend teardown runs outside lifecycle callback");
    CHECK(p.churn_emitted==4096 && p.stats.teardown_total==4112,
          "churn retires every lifetime including exits before attach");
    CHECK(global_attaches==2 && pid_attaches==0 && destroys==0,
          "only two global uprobes survive all backend exits");
    probe_close_uprobes(&p); probe_close_uprobes(&p);
    CHECK(destroys==2, "global uprobes destroyed once at shutdown");
    printf("probe churn: PASS (4096 arrivals at 1000/s, registry max_used=%"PRIu64" size=%u)\n",
        p.stats.registry_max_used, (unsigned)MAX_BACKENDS);
    return 0;
}
int main(void)
{
    if (churn_registry()) return 1;
    struct probe p; setup(&p);
    probe_service_backends(&p);
    CHECK(!p.overflow, "lifecycle ring drained between attach batches");
    CHECK(p.stats.attaches.total==32 && p.stats.attaches.max==1, "attach max/total counts actual operations");
    CHECK(p.stats.proc_reads.total==64 && p.stats.proc_reads.max==4, "proc max/total counts actual operations");
    CHECK(p.stats.lifecycle_peak==24000 && p.stats.lifecycle_samples==32,
          "lifecycle peak samples L before drain in bytes");
    p.stats=(struct probe_stats){0};
    probe_service_backends(&p);
    CHECK(!p.overflow && p.stats.proc_reads.total==64 && p.stats.proc_reads.max==4,
          "already instrumented proc work is drained per backend");
    CHECK(!p.stats.attaches.total, "already instrumented backend needs no attach");
    setup(&p); p.retire=1;
    probe_service_backends(&p);
    CHECK(p.backends[0].pid==999 && p.backends[0].pending && p.backends[0].wait==-1,
          "drain slot reuse rechecks pid and pending before wait attach");
    setup(&p); p.drain_error=1;
    probe_service_backends(&p);
    CHECK(p.failed && p.stats.attaches.total==1, "drain error stops attach work");
    setup(&p);
    for (int i=2; i<MAX_BACKENDS; ++i) p.backends[i]=(struct probe_backend){.boot=-1, .wait=-1};
    for (int i=0; i<2; ++i) {
        p.backends[i].boot=7; p.backends[i].wait=8;
    }
    /* Stop at consume return to inspect ownership before the drain's
     * cleanup batch, including two lifetimes sharing a reused PID. */
    p.backends[0].pending=false; p.emit_exits=true;
    ring_buffer__consume(p.rings);
    CHECK(!callback_teardown, "backend teardown runs outside lifecycle callback");
    CHECK(!p.failed && p.exited_before_attach==1, "EXIT identity and pending counter preserved");
    CHECK(p.backends[1].pid==2 && p.backends[1].cookie==2 && p.backends[1].closing &&
          p.backends[1].boot==7 && p.backends[1].wait==8,
          "closing slot reserved until deferred teardown");
    probe_teardown_one(&p);
    CHECK(p.stats.teardown_total==1 && closes==2 && destroys==0 && !p.backends[0].pid,
          "one watchpoint-only backend teardown preserves other slots");
    probe_instrument(&p, &p.backends[1]); probe_try_attach(&p, &p.backends[1]);
    CHECK(!p.stats.attaches.total && !p.stats.proc_reads.total && p.backends[1].pending,
          "closing backend never instrumented or attached");
    /* A new FORK for the same PID gets another slot, even before the old
     * cookie's fd/link teardown. Registration itself cannot do teardown. */
    in_callback=true; probe_new_backend(&p, 2, 33); in_callback=false;
    CHECK(!callback_teardown && p.backends[0].pid==2 && p.backends[0].cookie==33 &&
          p.backends[1].pid==2 && p.backends[1].cookie==2 && p.backends[1].closing,
          "same PID FORK preserves closing slot and registers a separate lifetime");
    probe_drain(&p);
    CHECK(p.stats.teardown_total==2 && !p.backends[1].pid && p.backends[0].cookie==33 &&
          closes==4 && destroys==0, "deferred teardown retires only the old lifetime");
    probe_drain(&p);
    CHECK(p.stats.consume_calls==2 && p.stats.consume_max_ns==25000 && p.stats.teardown_max_ns==2000,
          "consume and teardown timing cover each call separately");
    CHECK(p.stats.lifecycle_peak==0 && p.stats.lifecycle_callback_peak==44000,
          "every L callback samples occupancy while consume is running");
    probe_read_stats(&p);
    CHECK(p.stats.sites_valid && p.stats.site_drops[0]==11 && p.stats.site_drops[1]==13 &&
          p.stats.site_drops[2]==15 && p.stats.site_drops[3]==17, "site stats sum every CPU at named slots");
    p.stats.lifecycle_size=65536;
    probe_print_stats(&p.stats);
    counter_error=1; probe_read_stats(&p);
    CHECK(p.failed && !p.stats.sites_valid, "counter read error stays unavailable and fails probe");
    probe_print_stats(&p.stats);
    setup(&p); p.self=true;
    CHECK(probe_attach_uprobes(&p)==0 && !uprobe_calls, "self reuse attaches no PostgreSQL uprobes");
    setup(&p);
    CHECK(probe_attach_uprobes(&p)==0 && global_attaches==2 && pid_attaches==0 &&
          query_program==p.skel->progs.on_report_query_id, "PG17+ global query program selected");
    probe_close_uprobes(&p);
    setup(&p); p.skel->rodata->pg13_query_attr=1;
    CHECK(probe_attach_uprobes(&p)==0 && query_program==p.skel->progs.on_executor_start,
          "PG13 global query program selected");
    probe_close_uprobes(&p);
    for (int fail=1; fail<=2; ++fail) {
        setup(&p); fail_uprobe_at=fail;
        CHECK(probe_attach_uprobes(&p)<0 && p.failed, "global uprobe attach error fails probe");
        probe_close_uprobes(&p); probe_close_uprobes(&p);
        CHECK(destroys==fail-1, "partial global uprobe setup cleaned exactly once");
    }
    setup(&p);
    for (int i=16; i<MAX_BACKENDS; ++i) probe_new_backend(&p, (u32)i+1, (u64)i+1);
    for (int i=0; i<MAX_BACKENDS; ++i) p.backends[i].closing=true;
    probe_new_backend(&p, 12345, 12345);
    CHECK(p.failed && p.stats.registry_max_used==MAX_BACKENDS,
          "full registry fails with resource counts");
    puts("probe loop: PASS (bounded attaches/teardown, callback occupancy, timing, reserved slots, drain errors, statistics)");
    return 0;
}
