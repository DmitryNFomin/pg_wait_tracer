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
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", label); return 1; } } while (0)
/* PROBE_STATS */
static void probe_note_work(struct probe_work_stat *work);
struct bpf_link { int unused; };
struct probe_backend {
    u32 pid; bool pending, raced; int boot, wait;
    struct bpf_link *activity, *query;
};
struct fake_skel {
    struct { void *on_bootstrap, *on_report_activity, *on_executor_start, *on_report_query_id; } progs;
    struct { int pg13_query_attr; } *rodata;
    struct { void *ringbuf_site_drops; } maps;
};
struct ring { u64 consumer, producer; };
struct probe {
    struct probe_stats stats;
    struct probe_backend backends[MAX_BACKENDS];
    struct fake_skel *skel; void *rings, *perf;
    u64 ptr, activity_offset, query_offset;
    const char *exe; int failed, possible; bool loaded;
    int overflow, drain_error, retire, drains;
};
static struct ring lifecycle;
static struct probe *active;
static int ring_buffer__consume(void *rings)
{
    (void)rings;
    if (active->drain_error) return -1;
    lifecycle.consumer = lifecycle.producer;
    ++active->drains;
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
/* Every slow operation lets a producer add records. A drain after K=3
 * attaches fits the 64 KiB queue; deferring it over all backends loses data. */
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
    (void)program; (void)ret; (void)pid; (void)exe; (void)offset;
    traffic(active, 20000); return (struct bpf_link *)1;
}
static long libbpf_get_error(void *link) { (void)link; return 0; }
static void probe_attach_error(struct probe *p, struct probe_backend *b, const char *step, int error)
{ (void)b; (void)step; (void)error; p->failed=1; }
static void probe_try_attach(struct probe *p, struct probe_backend *b)
{
    if (!b->pid || b->pending) return;
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
    skel.rodata=&rodata;
    *p=(struct probe){.skel=&skel, .activity_offset=1, .query_offset=1, .possible=2, .loaded=true};
    lifecycle=(struct ring){0}; active=p;
    for (int i=0; i<16; ++i)
        p->backends[i]=(struct probe_backend){.pid=(u32)i+1, .pending=true, .boot=-1, .wait=-1};
}
int main(void)
{
    struct probe p; setup(&p);
    probe_service_backends(&p);
    CHECK(!p.overflow, "lifecycle ring drained between attach batches");
    CHECK(p.stats.attaches.total==64 && p.stats.attaches.max==3, "attach max/total counts actual operations");
    CHECK(p.stats.proc_reads.total==64 && p.stats.proc_reads.max==4, "proc max/total counts actual operations");
    CHECK(p.stats.lifecycle_peak==60000 && p.stats.lifecycle_samples==32,
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
    CHECK(p.failed && p.stats.attaches.total==3, "drain error stops attach work");
    setup(&p); probe_read_stats(&p);
    CHECK(p.stats.sites_valid && p.stats.site_drops[0]==11 && p.stats.site_drops[1]==13 &&
          p.stats.site_drops[2]==15 && p.stats.site_drops[3]==17, "site stats sum every CPU at named slots");
    p.stats.lifecycle_size=65536;
    probe_print_stats(&p.stats);
    counter_error=1; probe_read_stats(&p);
    CHECK(p.failed && !p.stats.sites_valid, "counter read error stays unavailable and fails probe");
    probe_print_stats(&p.stats);
    puts("probe loop: PASS (bounded attaches, proc scans, slot reuse, drain errors, statistics)");
    return 0;
}
