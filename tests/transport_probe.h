/* Standalone Linux A1 probe, included by the registered C test. No daemon,
 * merge core, capture writer or certification. Attach/seed below is stimulus
 * instrumentation only, deliberately not an A2 attachment proof. */
#ifndef PGWT_TRANSPORT_PROBE_H
#define PGWT_TRANSPORT_PROBE_H
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "pg_wait_tracer.skel.h"
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#include "transport_probe_request.h"

struct probe_identity {
    u32 pid; bool fork_seen, exit_seen;
    u64 records, fork_ts, exit_ts, min_ts, max_ts;
};
struct probe_pid_cookie { u32 pid; u64 cookie; };
struct probe_backend {
    u32 pid; u64 cookie; int boot, wait;
    bool init_seen;
    /* pending: registered at FORK consumption, not yet instrumented.
     * raced: an attach step met the exit race; nothing more is attempted.
     * closing: teardown queued; keep the slot occupied and never attach. */
    bool pending, raced, closing;
};
struct probe_helper {
    pthread_t thread; struct transport_request_channel channel;
    int cpu, error; atomic_int tid; struct probe *probe;
};
/* Falsifier only. A requester wakes the helpers of the OTHER CPUs and stamps
 * each wake. Two requesters are pinned to two different CPUs, so whichever
 * CPU a forced-failure spin occupies (the probe leader's included), at least
 * one requester is free to challenge that CPU's helper during the spin. */
struct probe_requester {
    pthread_t thread; int cpu, error; atomic_int ready; struct probe *probe;
};
struct probe_work_stat { u64 total, since_drain, max; };
struct probe_stats {
    struct probe_work_stat attaches, proc_reads;
    u64 lifecycle_peak, lifecycle_size, lifecycle_samples;
    u64 lifecycle_callback_peak, consume_max_ns, consume_calls;
    u64 teardown_max_ns, teardown_total, registry_max_used;
    u64 site_drops[4]; bool sites_valid;
};
static void probe_note_work(struct probe_work_stat *work)
{
    ++work->total;
    if (++work->since_drain > work->max) work->max = work->since_drain;
}
struct probe {
    struct probe_stats stats; bool loaded;
    struct pg_wait_tracer_bpf *skel;
    struct ring_buffer *rings; struct perf_buffer *perf;
    struct bpf_link *fork, *exit, *helper, *activity, *query;
    struct probe_identity *identities; size_t identities_cap;
    struct probe_pid_cookie *pid_lifetimes;
    struct probe_backend backends[MAX_BACKENDS];
    struct probe_helper *helpers;
    struct probe_requester requesters[2];
    int requesters_started; atomic_int requesters_stop;
    pthread_mutex_t request_lock;
    /* The INIT-publication mutant's own failed NMI reservations on L are
     * expected there; nothing else is tolerated (see probe_reconcile). */
    bool init_mutant;
    /* Control only: spin without the stamped challenges. The result must be
     * a refusal, which shows the evidence requirement can fire. */
    bool no_challenge;
    int possible, online, main_cpu, helpers_started, failed;
    u64 *delivered, *last_seq, *acks, *snapshot;
    u64 boundary_w[3]; u32 boundary_round[3];
    struct probe_stamp *helper_stamps, *requests;
    size_t helper_stamps_len, helper_stamps_cap, requests_len, requests_cap;
    u64 records[5], lost, forks, reuses, attached, init_records, init_stored;
    /* Backends that exited before or during instrumentation (counted once
     * each), and attach errors on a backend that was still live. */
    u64 attach_exit_races, attach_failures;
    /* Backends whose EXIT record was consumed before the main loop reached
     * them: gone before any attach was attempted. */
    u64 exited_before_attach;
    u64 *skipped, *discard_scratch;
    u64 activity_offset, query_offset, ptr; u32 wait_offset;
    const char *exe, *cohort_db; bool self, spinning, stopping; u64 spin_ns;
};
static volatile sig_atomic_t probe_stop;
static volatile u32 probe_wait;
static u64 probe_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000 + (u64)t.tv_nsec;
}
static void probe_signal(int sig) { (void)sig; probe_stop = 1; }
static int probe_pin(int cpu)
{
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask);
}
/* The open descriptor, or the negative errno of the step that failed. */
static int probe_watchpoint(struct probe *p, u32 pid, u64 addr, int program, bool enabled)
{
    struct perf_event_attr attr = { .type=PERF_TYPE_BREAKPOINT, .size=sizeof(attr),
        .bp_addr=addr, .bp_type=HW_BREAKPOINT_W, .bp_len=HW_BREAKPOINT_LEN_4,
        .sample_period=1, .disabled=1, .exclude_kernel=1, .exclude_hv=1 };
    probe_note_work(&p->stats.attaches);
    int fd = (int)syscall(__NR_perf_event_open, &attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) return errno ? -errno : -EIO;
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, program) ||
        (enabled && ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))) {
        int error = errno ? errno : EIO; close(fd); return -error;
    }
    return fd;
}
static void probe_close_backend(struct probe_backend *b)
{
    if (b->boot >= 0) close(b->boot);
    if (b->wait >= 0) close(b->wait);
    memset(b, 0, sizeof(*b)); b->boot = b->wait = -1;
}
static void probe_fail(struct probe *p, const char *why)
{
    fprintf(stderr, "PROBE-FAIL: %s\n", why); p->failed = 1;
}
static bool probe_task_live(struct probe *p, u32 pid)
{
    char name[64], text[768]; snprintf(name, sizeof(name), "/proc/%u/stat", pid);
    int fd = open(name, O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    probe_note_work(&p->stats.proc_reads);
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
    if (n <= 0) return false;
    text[n] = '\0';
    return probe_stat_live(text) == 1;
}
/* One attach step of backend b failed with `error` (a positive errno). The
 * verdict is probe_attach_is_exit_race (transport_probe_decode.h): ESRCH, or
 * a task that is no longer live, is the exit race: counted once per backend,
 * reported in the summary, never a probe failure. Anything else happened on
 * a live backend: it is printed with its errno and fails the probe. */
static void probe_attach_error(struct probe *p, struct probe_backend *b, const char *step, int error)
{
    if (probe_attach_is_exit_race(error, probe_task_live(p, b->pid))) {
        if (!b->raced) { b->raced = true; ++p->attach_exit_races; }
        return;
    }
    ++p->attach_failures;
    fprintf(stderr, "PROBE-ATTACH-FAIL: %s pid=%u cookie=%"PRIu64" errno=%d (%s) on a live backend\n",
        step, b->pid, b->cookie, error, strerror(error));
    probe_fail(p, "attach failed on a live backend");
}
static struct probe_identity *probe_identity(struct probe *p, u64 cookie, u32 pid)
{
    if (!cookie || cookie > 1000000) { probe_fail(p, "absent/oversized lifetime cookie"); return NULL; }
    if (cookie >= p->identities_cap) {
        size_t cap = p->identities_cap ? p->identities_cap : 1024;
        while (cookie >= cap) cap *= 2;
        void *next = realloc(p->identities, cap * sizeof(*p->identities));
        if (!next) { probe_fail(p, "identity allocation"); return NULL; }
        p->identities = next;
        memset(p->identities+p->identities_cap, 0, (cap-p->identities_cap)*sizeof(*p->identities));
        p->identities_cap = cap;
    }
    struct probe_identity *id = &p->identities[cookie];
    if (id->pid && id->pid != pid) probe_fail(p, "cookie reissued to another PID");
    id->pid = pid; return id;
}
static void probe_observe_stored_init(struct probe *p, struct probe_backend *b)
{
    if (b->init_seen || !p->cohort_db) return;
    u32 key=b->pid; struct pgwt_pid_state st;
    if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.state_map), &key, &st) ||
        st.cookie != b->cookie || !st.init_stored) return;
    /* The driver owns a unique database. Bind the nonvacuity count to its
     * cohort, rather than unrelated postmaster children. PostgreSQL updates
     * this title after startup, so keep observing already attached backends. */
    char name[64], title[512], needle[128];
    snprintf(name,sizeof(name),"/proc/%u/cmdline",b->pid);
    int fd=open(name,O_RDONLY|O_CLOEXEC); if (fd<0) return;
    probe_note_work(&p->stats.proc_reads);
    ssize_t n=read(fd,title,sizeof(title)-1); close(fd); if (n<=0) return;
    for (ssize_t i=0;i<n;++i) if (!title[i]) title[i]=' ';
    title[n]='\0'; snprintf(needle,sizeof(needle)," %s ",p->cohort_db);
    if (strstr(title,needle)) {
        b->init_seen=true; ++p->init_stored;
        fprintf(stderr,"INIT-STORED pid=%u cookie=%"PRIu64"\n",b->pid,b->cookie);
    }
}
/* The daemon's own predicate (src/discovery.c pgwt_addr_is_shared, used by
 * src/backend.c for a direct attach): the wait pointer counts only when it
 * targets a MAP_SHARED mapping. On PG17+ my_wait_event_info is non-zero from
 * the first instruction, pointing at a process-local placeholder until
 * InitProcess() switches it to the PGPROC field in shared memory, so a
 * non-zero value alone proves nothing. Returns 1 only for a shared target. */
static int probe_addr_is_shared(struct probe *p, u32 pid, u64 addr)
{
    char name[64], line[512]; snprintf(name, sizeof(name), "/proc/%u/maps", pid);
    FILE *maps=fopen(name, "r"); if (!maps) return -1;
    int result=0;
    while (true) {
        probe_note_work(&p->stats.proc_reads);
        if (!fgets(line, sizeof(line), maps)) break;
        unsigned long long start=0, end=0; char perms[8]="";
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        if (addr < start) break;
        if (addr >= end) continue;
        result=perms[3] == 's'; break;
    }
    fclose(maps); return result;
}
static void probe_try_attach(struct probe *p, struct probe_backend *b)
{
    if (!b->pid || b->pending || b->closing || p->self || p->stopping) return;
    probe_observe_stored_init(p,b);
    /* Exited before or during instrumentation: no A2 coverage claim, and
     * nothing more to try until its EXIT record frees the slot. */
    if (b->wait >= 0 || b->raced) return;
    char name[64]; snprintf(name, sizeof(name), "/proc/%u/mem", b->pid);
    int mem = open(name, O_RDONLY | O_CLOEXEC);
    if (mem < 0) { probe_attach_error(p, b, "open /proc/<pid>/mem", errno); return; }
    u64 ptr = 0;
    probe_note_work(&p->stats.proc_reads);
    ssize_t n = pread(mem, &ptr, sizeof(ptr), (off_t)p->ptr);
    if (n != sizeof(ptr)) {
        int error = n < 0 ? errno : EIO; close(mem);
        probe_attach_error(p, b, "read the wait pointer", error); return;
    }
    if (!ptr) { close(mem); return; }
    /* Not initialised yet: leave the armed bootstrap watchpoint in place. */
    int shared = probe_addr_is_shared(p, b->pid, ptr);
    if (shared != 1) {
        int error = errno; close(mem);
        if (shared < 0) probe_attach_error(p, b, "read /proc/<pid>/maps", error ? error : ENOENT);
        return;
    }
    u64 addr = ptr + p->wait_offset;
    u32 wait = 0;
    probe_note_work(&p->stats.proc_reads);
    n = pread(mem, &wait, sizeof(wait), (off_t)addr);
    int read_error = n < 0 ? errno : EIO; close(mem);
    if (n != sizeof(wait)) { probe_attach_error(p, b, "read the wait word", read_error); return; }
    int fd = probe_watchpoint(p, b->pid, addr, bpf_program__fd(p->skel->progs.on_watchpoint), false);
    if (fd < 0) { probe_attach_error(p, b, "open the wait watchpoint", -fd); return; }
    u32 key = b->pid; struct pgwt_pid_state st;
    int map = bpf_map__fd(p->skel->maps.state_map);
    /* The producer retires the state at exit, and a reused PID carries
     * another cookie: either way this lifetime is over. */
    if (bpf_map_lookup_elem(map, &key, &st) || st.cookie != b->cookie) {
        close(fd); probe_attach_error(p, b, "fork-seeded state", ESRCH); return;
    }
    st.wait_event_addr = addr; st.last_event = wait; st.last_ts = probe_now(); st.wp_live = 1;
    if (bpf_map_update_elem(map, &key, &st, BPF_ANY) || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0)) {
        int error = errno ? errno : EIO; close(fd);
        probe_attach_error(p, b, "seed/enable the wait watchpoint", error); return;
    }
    b->wait = fd; ++p->attached;
    if (b->boot >= 0) { close(b->boot); b->boot = -1; }
}
static void probe_new_backend(struct probe *p, u32 pid, u64 cookie)
{
    if (p->self || p->stopping) return;
    struct probe_backend *slot = NULL;
    unsigned used=0, closing=0, pending=0, boot=0, wait=0;
    for (int i=0; i<MAX_BACKENDS; ++i) {
        struct probe_backend *b = &p->backends[i];
        if (b->pid == pid) b->closing = true;
        if (!b->pid) { if (!slot) slot=b; continue; }
        ++used;
        closing += b->closing;
        pending += b->pending && !b->closing;
        boot += b->boot >= 0;
        wait += b->wait >= 0;
    }
    if (!slot) {
        fprintf(stderr, "PROBE-FAIL: backend slots exhausted used=%u size=%u closing=%u pending=%u active=%u boot_fds=%u wait_fds=%u\n",
            used, (unsigned)MAX_BACKENDS, closing, pending, used-closing-pending, boot, wait);
        p->failed=1; return;
    }
    /* Registration only inside the lifecycle callback. Uprobes belong to
     * the whole probe; only per-backend watchpoints are deferred to the
     * main loop. No slow uprobe link destruction can hold these slots. */
    slot->pid = pid; slot->cookie = cookie; slot->boot = slot->wait = -1; slot->pending = true;
    if (++used > p->stats.registry_max_used) p->stats.registry_max_used = used;
}
/* Main loop only, never from a ring callback. */
static void probe_instrument(struct probe *p, struct probe_backend *b)
{
    if (!b->pid || !b->pending || b->closing) return;
    b->pending = false;
    int fd = probe_watchpoint(p, b->pid, p->ptr, bpf_program__fd(p->skel->progs.on_bootstrap), true);
    if (fd < 0) { b->boot = -1; probe_attach_error(p, b, "open the bootstrap watchpoint", -fd); }
    else b->boot = fd;
}
/* Match the daemon's exact_probe.c: one link per symbol, all processes of
 * this executable. Identity checks use each wire record's PID and cookie,
 * never an attach-time PID or current BPF map entry. */
static int probe_attach_uprobes(struct probe *p)
{
    if (p->self) return 0;
    if (p->activity_offset) {
        probe_note_work(&p->stats.attaches);
        p->activity = bpf_program__attach_uprobe(p->skel->progs.on_report_activity, false,
            -1, p->exe, (size_t)p->activity_offset);
        long error = libbpf_get_error(p->activity);
        if (error) { p->activity=NULL; probe_fail(p, "attach global activity uprobe"); return -1; }
    }
    if (p->query_offset) {
        struct bpf_program *program = p->skel->rodata->pg13_query_attr
            ? p->skel->progs.on_executor_start : p->skel->progs.on_report_query_id;
        probe_note_work(&p->stats.attaches);
        p->query = bpf_program__attach_uprobe(program, false, -1, p->exe, (size_t)p->query_offset);
        long error = libbpf_get_error(p->query);
        if (error) { p->query=NULL; probe_fail(p, "attach global query uprobe"); return -1; }
    }
    return 0;
}
static void probe_close_uprobes(struct probe *p)
{
    bpf_link__destroy(p->activity); p->activity=NULL;
    bpf_link__destroy(p->query); p->query=NULL;
}

static int probe_record(struct probe *p, int dest, const void *data, size_t size, int perf_cpu)
{
    struct pgwt_wire_hdr hdr;
    if (size < sizeof(hdr)) { probe_fail(p, "short envelope"); return 0; }
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.version != PGWT_WIRE_VERSION || hdr.flags || hdr.q ||
        hdr.cpu >= p->possible || size != sizeof(hdr)+hdr.payload_len ||
        (perf_cpu >= 0 && hdr.cpu != perf_cpu) || hdr.kind < 1 || hdr.kind > 4) {
        probe_fail(p, "wire framing / CPU / N mode"); return 0;
    }
    size_t index = (size_t)dest * p->possible + hdr.cpu;
    /* A failed attempt consumes a number, so the arms that expect failures
     * see gaps; the reconcile step accounts for every one of them. */
    bool gaps_expected = p->spinning || (p->init_mutant && dest == PGWT_WIRE_DEST_L);
    if (hdr.seq > p->last_seq[index]+1 && !gaps_expected) {
        /* A skipped number is an attempt that delivered nothing. Only a
         * discard the producer counted (before releasing its reservation)
         * explains it; anything else is a gap. */
        u32 key = (u32)dest; u64 counted = 0;
        if (!bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_discards), &key, p->discard_scratch))
            counted = p->discard_scratch[hdr.cpu];
        p->skipped[index] += hdr.seq - p->last_seq[index] - 1;
        if (p->skipped[index] > counted) probe_fail(p, "attempt sequence gap/reorder");
    }
    if (hdr.seq <= p->last_seq[index]) probe_fail(p, "duplicate/nonincreasing attempt");
    p->last_seq[index] = hdr.seq; ++p->delivered[index]; ++p->records[hdr.kind];
    const unsigned char *payload = (const unsigned char *)data + sizeof(hdr);
    u64 timestamp = 0; u32 pid = 0;
    if (hdr.kind == PGWT_WIRE_BOUNDARY) {
        struct pgwt_wire_boundary cut;
        if (hdr.payload_len != sizeof(cut) || hdr.cookie) { probe_fail(p, "boundary framing"); return 0; }
        memcpy(&cut, payload, sizeof(cut));
        if (cut.dest != (u32)dest || !cut.round) probe_fail(p, "boundary destination/round");
        if (cut.w < p->boundary_w[dest] && dest != PGWT_WIRE_DEST_CPU) probe_fail(p, "boundary clock regression");
        if (dest != PGWT_WIRE_DEST_CPU) {
            p->boundary_w[dest] = cut.w; p->boundary_round[dest] = cut.round;
        }
        else {
            p->acks[hdr.cpu] = cut.round;
            if (p->spinning) {
                if (p->helper_stamps_len == p->helper_stamps_cap) {
                    size_t cap=p->helper_stamps_cap ? p->helper_stamps_cap*2 : 1024;
                    void *next=realloc(p->helper_stamps, cap*sizeof(*p->helper_stamps));
                    if (!next) { probe_fail(p,"falsifier stamp allocation"); return 0; }
                    p->helper_stamps=next; p->helper_stamps_cap=cap;
                }
                p->helper_stamps[p->helper_stamps_len++]=(struct probe_stamp){
                    .cpu=hdr.cpu, .round=cut.round, .stamp=cut.w};
            }
        }
        return 0;
    }
    if (hdr.kind == PGWT_WIRE_TRACE && hdr.payload_len == sizeof(struct pgwt_trace_event)) {
        struct pgwt_trace_event ev; memcpy(&ev, payload, sizeof(ev));
        timestamp = ev.timestamp_ns; pid = ev.pid;
        if (dest == PGWT_WIRE_DEST_L || (dest == PGWT_WIRE_DEST_E &&
            ev.new_event != PGWT_EVENT_EXIT && ev.old_event != ev.new_event))
            probe_fail(p,"TRACE routed to wrong destination");
        if (dest == PGWT_WIRE_DEST_CPU && (ev.new_event == PGWT_EVENT_EXIT || ev.new_event == ev.old_event))
            probe_fail(p, "non-watchpoint trace routed to perf");
    } else if (hdr.kind == PGWT_WIRE_LIFECYCLE && hdr.payload_len == sizeof(struct pgwt_lifecycle_event) && dest == 2) {
        struct pgwt_lifecycle_event ev; memcpy(&ev, payload, sizeof(ev));
        timestamp = ev.timestamp; pid = ev.pid;
        struct probe_identity *id = probe_identity(p, hdr.cookie, pid);
        if (id && ev.type == PGWT_LIFECYCLE_FORK) {
            if (id->fork_seen) probe_fail(p, "duplicate FORK lifetime");
            /* All previously consumed lifetimes stay in this cookie table,
             * including after their PID is reused. No current map resolution. */
            size_t hash=((size_t)pid*2654435761U) & ((1U<<20)-1);
            while (p->pid_lifetimes[hash].pid && p->pid_lifetimes[hash].pid != pid)
                hash=(hash+1)&((1U<<20)-1);
            if (p->pid_lifetimes[hash].pid && p->pid_lifetimes[hash].cookie != hdr.cookie) ++p->reuses;
            p->pid_lifetimes[hash]=(struct probe_pid_cookie){.pid=pid,.cookie=hdr.cookie};
            id->fork_seen = true; id->fork_ts=ev.timestamp;
            ++p->forks; probe_new_backend(p, pid, hdr.cookie);
        } else if (id && !p->spinning && (!id->fork_seen || id->exit_seen)) {
            probe_fail(p,"L record outside its own FORK/EXIT positions");
        }
        if (id && ev.type == PGWT_LIFECYCLE_EXIT) {
            if (id->exit_seen) probe_fail(p,"duplicate lifetime EXIT");
            id->exit_seen=true; id->exit_ts=ev.timestamp;
        }
        if (ev.type == PGWT_LIFECYCLE_INIT) {
            /* Restoration mutant only. Keep even its /proc and attach work
             * in the main loop, never inside the lifecycle drain callback. */
            ++p->init_records;
        }
        if (ev.type == PGWT_LIFECYCLE_EXIT) for (int i=0; i<MAX_BACKENDS; ++i)
            if (p->backends[i].pid == pid && p->backends[i].cookie == hdr.cookie &&
                !p->backends[i].closing) {
                if (p->backends[i].pending) ++p->exited_before_attach;
                p->backends[i].closing = true;
            }
    } else if (hdr.kind == PGWT_WIRE_TEXT && hdr.payload_len == sizeof(struct pgwt_query_text_event) && dest == 2) {
        struct pgwt_query_text_event ev; memcpy(&ev, payload, sizeof(ev)); pid = ev.pid;
    } else { probe_fail(p, "wrong kind/payload/destination"); return 0; }
    if (dest != PGWT_WIRE_DEST_CPU && timestamp && timestamp < p->boundary_w[dest])
        probe_fail(p, "record timestamp before consumed E/L boundary");
    if (!pid) probe_fail(p,"zero backend PID");
    struct probe_identity *id = probe_identity(p, hdr.cookie, pid);
    if (id) {
        if (!p->spinning && hdr.kind==PGWT_WIRE_TEXT && (!id->fork_seen || id->exit_seen))
            probe_fail(p,"TEXT outside its own lifetime L positions");
        ++id->records;
        if (timestamp && (!id->min_ts || timestamp<id->min_ts)) id->min_ts=timestamp;
        if (timestamp>id->max_ts) id->max_ts=timestamp;
    }
    return 0;
}
static int probe_e(void *ctx, void *data, size_t size) { return probe_record(ctx, 1, data, size, -1); }
static int probe_l(void *ctx, void *data, size_t size)
{
    struct probe *p = ctx;
    /* Every L record, including TEXT/boundaries and records that fail a
     * check. The consumer position still includes the current record. */
    struct ring *lifecycle = ring_buffer__ring(p->rings, 1);
    if (lifecycle) {
        u64 consumed = ring__consumer_pos(lifecycle);
        u64 distance = ring__producer_pos(lifecycle) - consumed;
        if (distance > p->stats.lifecycle_callback_peak) p->stats.lifecycle_callback_peak = distance;
    }
    return probe_record(p, 2, data, size, -1);
}
static void probe_sample(void *ctx, int cpu, void *data, __u32 size)
{
    size_t wire=probe_perf_wire_size(data, size);
    if (!wire) { probe_fail(ctx,"PERF_SAMPLE_RAW padded length"); return; }
    probe_record(ctx, 0, data, wire, cpu);
}
static void probe_lost(void *ctx, int cpu, __u64 count)
{ (void)cpu; struct probe *p=ctx; p->lost+=count; probe_fail(p, "PERF_RECORD_LOST"); }
static void *probe_helper_loop(void *arg)
{
    struct probe_helper *h = arg;
    h->error = probe_pin(h->cpu) ? errno : 0;
    int policy=-1; struct sched_param scheduling;
    int scheduling_rc=pthread_getschedparam(pthread_self(),&policy,&scheduling);
    if (!h->error && (scheduling_rc || policy != SCHED_OTHER)) h->error=scheduling_rc?scheduling_rc:EINVAL;
    atomic_store(&h->tid, (int)syscall(SYS_gettid));
    /* One syscall per wake, in every arm. The falsifier's requesters wake
     * this thread many times per millisecond, so it is runnable on its CPU
     * throughout a spin there without flooding the perf buffer. */
    while (transport_request_wait(&h->channel))
        if (!h->error) syscall(SYS_getppid);
    return NULL;
}
static void *probe_requester_loop(void *arg)
{
    struct probe_requester *r = arg; struct probe *p = r->probe;
    r->error = probe_pin(r->cpu) ? errno : 0;
    atomic_store(&r->ready, 1);
    u32 zero = 0; int fd = bpf_map__fd(p->skel->maps.transport_ctl);
    while (!atomic_load(&p->requesters_stop) && !r->error) {
        struct pgwt_transport_ctl ctl;
        if (!bpf_map_lookup_elem(fd, &zero, &ctl) && ctl.round) {
            for (int i = 0; i < p->helpers_started; ++i) {
                /* While this thread runs, its own CPU is not spinning. */
                if (p->helpers[i].cpu == r->cpu) continue;
                struct probe_stamp stamp = { .cpu = (u32)p->helpers[i].cpu, .round = ctl.round };
                stamp.issued = probe_now();
                transport_request_signal(&p->helpers[i].channel);
                stamp.stamp = probe_now();
                pthread_mutex_lock(&p->request_lock);
                if (p->requests_len == p->requests_cap) {
                    size_t cap = p->requests_cap ? p->requests_cap * 2 : 4096;
                    void *next = realloc(p->requests, cap * sizeof(*p->requests));
                    if (next) { p->requests = next; p->requests_cap = cap; }
                    else r->error = ENOMEM;
                }
                if (!r->error) p->requests[p->requests_len++] = stamp;
                pthread_mutex_unlock(&p->request_lock);
            }
        }
        /* Dense on purpose: a spin lasts 8 ms or more and many challenges
         * must fall inside it. The boundaries this produces are why the
         * falsifier arm has a large perf buffer (see transport_probe). */
        usleep(100);
    }
    return NULL;
}
/* Main thread only, after consume returns. Each backend teardown closes
 * only bootstrap/wait watchpoint descriptors, with no uprobe RCU waits.
 * The slot remains occupied until both descriptors have been closed. */
static bool probe_teardown_one(struct probe *p)
{
    for (int i=0; i<MAX_BACKENDS; ++i) if (p->backends[i].closing) {
        u64 start = probe_now();
        probe_close_backend(&p->backends[i]);
        u64 elapsed = probe_now() - start;
        if (elapsed > p->stats.teardown_max_ns) p->stats.teardown_max_ns = elapsed;
        ++p->stats.teardown_total;
        return true;
    }
    return false;
}
static int probe_drain(struct probe *p)
{
    /* Sample before EVERY drain, including the top of the main loop and
     * completion loops. Only this thread advances the consumer position. */
    struct ring *lifecycle = ring_buffer__ring(p->rings, 1);
    if (lifecycle) {
        u64 consumed = ring__consumer_pos(lifecycle);
        u64 distance = ring__producer_pos(lifecycle) - consumed;
        if (distance > p->stats.lifecycle_peak) p->stats.lifecycle_peak = distance;
        ++p->stats.lifecycle_samples;
    }
    u64 start = probe_now();
    int a = ring_buffer__consume(p->rings);
    u64 elapsed = probe_now() - start;
    if (elapsed > p->stats.consume_max_ns) p->stats.consume_max_ns = elapsed;
    ++p->stats.consume_calls;
    int b = perf_buffer__consume(p->perf);
    p->stats.attaches.since_drain = p->stats.proc_reads.since_drain = 0;
    /* Retire all consumed EXITs: cheap descriptor closes cannot accumulate
     * behind a count-limited queue of uprobe link RCU waits. */
    while (probe_teardown_one(p)) {}
    if (a < 0 || b < 0) { probe_fail(p, "buffer consume"); return -1; }
    return a+b;
}
/* At most one backend watchpoint attach per drain.
 * The wait attach and /proc inspection get a separate drain, even for an
 * already instrumented backend. Draining can retire/reuse b's slot: never
 * retain a PID, cookie or fd across it. try_attach rechecks pending/pid and
 * closing. A closing slot is never instrumented or attached. */
static void probe_service_backends(struct probe *p)
{
    for (int i=0; i<MAX_BACKENDS && !p->failed; ++i) {
        struct probe_backend *b = &p->backends[i];
        if (!b->pid) continue;
        if (b->closing) {
            if (probe_drain(p) < 0) return;
            continue;
        }
        if (b->pending) {
            probe_instrument(p, b);
            if (probe_drain(p) < 0 || p->failed) return;
        }
        probe_try_attach(p, b);
        if (probe_drain(p) < 0) return;
    }
}
static int probe_round(struct probe *p, bool el)
{
    u32 zero=0; struct pgwt_transport_ctl ctl;
    int fd=bpf_map__fd(p->skel->maps.transport_ctl);
    if (bpf_map_lookup_elem(fd, &zero, &ctl)) return -1;
    ++ctl.round; if (el) ++ctl.el_request;
    if (bpf_map_update_elem(fd, &zero, &ctl, BPF_ANY)) return -1;
    /* The falsifier's stamped challenges come from probe_requester_loop, not
     * from this thread: it is pinned to one CPU and cannot run while a spin
     * occupies that CPU. */
    for (int i=0; i<p->helpers_started; ++i)
        transport_request_signal(&p->helpers[i].channel);
    syscall(SYS_getppid); /* registered process leader publishes E/L */
    return 0;
}
static int probe_self_child(struct probe *p, int *reused)
{
    int channel[2]; if (pipe(channel)) return -1;
    pid_t child=fork();
    if (child < 0) { close(channel[0]); close(channel[1]); return -1; }
    if (!child) {
        close(channel[1]); char token;
        if (read(channel[0], &token, 1) != 1) _exit(2);
        for (u32 i=0; i<64; ++i) probe_wait = (i & 1) + 1;
        _exit(0);
    }
    close(channel[0]);
    u32 key=(u32)child; struct pgwt_pid_state state;
    int map=bpf_map__fd(p->skel->maps.state_map);
    int fd=probe_watchpoint(p, key, (u64)(uintptr_t)&probe_wait, bpf_program__fd(p->skel->progs.on_watchpoint), false);
    if (fd < 0 || bpf_map_lookup_elem(map, &key, &state)) {
        if (fd >= 0) close(fd);
        close(channel[1]); waitpid(child, NULL, 0); return -1;
    }
    state.wait_event_addr=(u64)(uintptr_t)&probe_wait; state.last_ts=probe_now(); state.wp_live=1;
    int rc=bpf_map_update_elem(map, &key, &state, BPF_ANY) || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    if (write(channel[1], "x", 1) != 1) rc=-1;
    close(channel[1]); int status; while (waitpid(child, &status, 0)<0) { if (errno != EINTR) { status=0; rc=-1; break; } } close(fd);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) rc=-1;
    if (*reused && *reused != child) {
        fprintf(stderr,"PROBE-REFUSED: concurrent allocation consumed forced PID; actual reuse missing\n"); rc=78;
    }
    *reused=child; return rc;
}
static int probe_self_reuse(struct probe *p)
{
    /* This test explicitly moves the allocation cursor on a disposable box.
     * It does not lower pid_max, change a live PID, or reset the BPF allocator.
     * Do not substitute local-namespace reuse: wire PIDs are initial-namespace
     * PIDs. A nested PID namespace is refused by the Python driver. */
    int reused=0;
    if (p->spinning) for (int i=0;i<p->helpers_started;++i)
        transport_request_signal(&p->helpers[i].channel);
    if (probe_self_child(p, &reused)) return -1;
    FILE *cursor=fopen("/proc/sys/kernel/ns_last_pid", "w");
    if (!cursor) { fprintf(stderr, "PROBE-REFUSED: ns_last_pid write: %s\n", strerror(errno)); return 78; }
    fprintf(cursor, "%d\n", reused-1);
    if (fclose(cursor)) return 78;
    /* Deliberately defer draining: both lifetimes' records must carry their
     * own cookie after the second fork has overwritten the PID entry. */
    return probe_self_child(p, &reused);
}
static int probe_link(struct bpf_link **out, struct bpf_program *program, const char *category, const char *name)
{
    *out=bpf_program__attach_tracepoint(program, category, name);
    long rc=libbpf_get_error(*out); if (rc) { *out=NULL; return (int)rc; }
    return *out ? 0 : -errno;
}
static int probe_start_helpers(struct probe *p)
{
    cpu_set_t mask; if (sched_getaffinity(0, sizeof(mask), &mask)) return -1;
    int online=(int)sysconf(_SC_NPROCESSORS_ONLN);
    if (CPU_COUNT(&mask) != online || online != p->online || p->possible > CPU_SETSIZE) {
        fprintf(stderr, "PROBE-REFUSED: must cover every online CPU with fixed membership\n"); return 78;
    }
    p->main_cpu=-1;
    for (int i=0; i<p->possible; ++i) if (CPU_ISSET(i, &mask)) { p->main_cpu=i; break; }
    if (p->main_cpu < 0 || probe_pin(p->main_cpu)) return -1;
    u32 tid=(u32)getpid(), cpu=(u32)p->main_cpu;
    if (bpf_map_update_elem(bpf_map__fd(p->skel->maps.helper_cpus), &tid, &cpu, BPF_ANY)) return -1;
    p->helpers=calloc((size_t)p->possible, sizeof(*p->helpers));
    if (!p->helpers) return -1;
    /* Include the leader's CPU too: another task must try to run there while
     * the leader executes the synthetic fork-failure falsifier. */
    for (int i=0; i<p->possible; ++i) if (CPU_ISSET(i, &mask)) {
        struct probe_helper *h=&p->helpers[p->helpers_started]; h->cpu=i; h->probe=p;
        atomic_init(&h->tid, 0);
        if (transport_request_init(&h->channel) || pthread_create(&h->thread, NULL, probe_helper_loop, h)) return -1;
        ++p->helpers_started;
        u64 limit=probe_now()+5000000000ULL;
        while (!atomic_load(&h->tid) && probe_now()<limit) sched_yield();
        if (!atomic_load(&h->tid) || h->error) return -1;
        tid=(u32)atomic_load(&h->tid); cpu=(u32)i;
        if (bpf_map_update_elem(bpf_map__fd(p->skel->maps.helper_cpus), &tid, &cpu, BPF_ANY)) return -1;
    }
    if (!p->spinning || p->no_challenge) return 0;
    /* Two requesters on two different CPUs: see struct probe_requester. With
     * one CPU a spin could never be challenged, so that is a refusal. */
    if (p->helpers_started < 2) {
        fprintf(stderr, "PROBE-REFUSED: the falsifier needs two online CPUs to challenge a spin\n"); return 78;
    }
    for (int i=0; i<2; ++i) {
        struct probe_requester *r=&p->requesters[i]; r->cpu=p->helpers[i].cpu; r->probe=p;
        atomic_init(&r->ready, 0);
        if (pthread_create(&r->thread, NULL, probe_requester_loop, r)) return -1;
        ++p->requesters_started;
        u64 limit=probe_now()+5000000000ULL;
        while (!atomic_load(&r->ready) && probe_now()<limit) sched_yield();
        if (!atomic_load(&r->ready) || r->error) return -1;
    }
    return 0;
}
static void probe_finish(struct probe *p)
{
    p->stopping=true;
    atomic_store(&p->requesters_stop, 1);
    for (int i=0; i<p->requesters_started; ++i) {
        pthread_join(p->requesters[i].thread, NULL);
        if (p->requesters[i].error) probe_fail(p, "falsifier requester");
    }
    p->requesters_started=0;
    for (int i=0; i<p->helpers_started; ++i) transport_request_stop(&p->helpers[i].channel);
    for (int i=0; i<p->helpers_started; ++i) { pthread_join(p->helpers[i].thread, NULL); transport_request_destroy(&p->helpers[i].channel); }
    p->helpers_started=0;
    /* Stop global producers once, before the final drains/reconciliation. */
    probe_close_uprobes(p);
    bpf_link__destroy(p->helper); p->helper=NULL;
    bpf_link__destroy(p->fork); p->fork=NULL;
    for (int i=0; i<MAX_BACKENDS; ++i)
        if (p->backends[i].pid) p->backends[i].closing=true;
    /* Shutdown uses the same cheap descriptor cleanup; no saved identity crosses a drain.
     * A partial setup without consumers has no producer links to service. */
    for (int i=0; i<MAX_BACKENDS; ++i) if (p->backends[i].closing) {
        if (p->rings && p->perf) probe_drain(p);
        else probe_teardown_one(p);
    }
    bpf_link__destroy(p->exit); p->exit=NULL;
    if (p->rings && p->perf) while (probe_drain(p)>0) {}
}
static int probe_reconcile(struct probe *p)
{
    u64 *counts=calloc((size_t)p->possible, sizeof(u64)); if (!counts) return -1;
    bool ok=!p->failed, refused=false; u64 total=0, failed=0;
    u64 *fail_counts=calloc((size_t)p->possible,sizeof(u64)); if (!fail_counts) { free(counts); return 1; }
    /* Attempts withdrawn by design (transport_discards): nothing was lost, so
     * every arm accepts them. attempts = delivered + failures + discards. */
    u64 *disc_counts=calloc((size_t)p->possible,sizeof(u64)), discarded=0;
    if (!disc_counts) { free(counts); free(fail_counts); return 1; }
    /* The INIT-publication mutant reserves on L in NMI context, where the
     * 4.18 ring lock is try-once, so some of ITS reservations fail. Each one
     * is counted at the INIT site on the CPU where it failed. Under
     * --init-mutant exactly those are accepted on L, per CPU; any other
     * failure, on any destination, still fails the run. */
    u64 *init_site=calloc((size_t)p->possible,sizeof(u64)), init_failures=0;
    u32 init_slot=PGWT_RING_FAIL_LIFECYCLE_INIT_HINT;
    if (!init_site) { free(counts); free(fail_counts); free(disc_counts); return 1; }
    if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.ringbuf_site_drops), &init_slot, init_site)) ok=false;
    printf("{\"cells\":[");
    for (u32 dest=0; dest<3; ++dest) {
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_attempts), &dest, counts)) { ok=false; break; }
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_failures), &dest, fail_counts)) { ok=false; break; }
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_discards), &dest, disc_counts)) { ok=false; break; }
        bool tolerated=p->init_mutant && dest == PGWT_WIRE_DEST_L;
        for (int cpu=0; cpu<p->possible; ++cpu) {
            size_t index=(size_t)dest*p->possible+cpu;
            u64 delivered=p->delivered[index]; total+=counts[cpu];
            u64 expected_missing=disc_counts[cpu] + ((p->spinning || tolerated) ? fail_counts[cpu] : 0);
            if (counts[cpu] != delivered+expected_missing || p->last_seq[index]>counts[cpu]) ok=false;
            if (!p->spinning && !tolerated && counts[cpu]-p->last_seq[index] > disc_counts[cpu]) ok=false;
            discarded+=disc_counts[cpu];
            if (tolerated) {
                if (fail_counts[cpu] != init_site[cpu]) ok=false;
                init_failures+=fail_counts[cpu];
            } else failed+=fail_counts[cpu];
            printf("%s{\"dest\":%u,\"cpu\":%d,\"attempted\":%"PRIu64",\"delivered\":%"PRIu64",\"failed\":%"PRIu64",\"discarded\":%"PRIu64",\"last_seq\":%"PRIu64"}", dest||cpu?",":"", dest, cpu, counts[cpu], delivered, fail_counts[cpu], disc_counts[cpu], p->last_seq[index]);
        }
    }
    free(init_site);
    for (size_t i=1; i<p->identities_cap; ++i) if (p->identities[i].records && !p->spinning) {
        struct probe_identity *id=&p->identities[i];
        if (!id->fork_seen || (id->min_ts && id->min_ts<id->fork_ts) ||
            (id->exit_seen && id->max_ts>id->exit_ts)) {
            probe_fail(p,"record cookie does not match its FORK-to-EXIT lifetime"); ok=false;
        }
    }
    if (!p->records[PGWT_WIRE_TRACE] || !p->forks || p->lost || (!p->spinning && failed)) ok=false;
    bool spun=false; u64 minimum_spin=~(u64)0; int spin_cpus=0;
    if (p->spinning) {
#ifdef PGWT_TRANSPORT_FALSIFIER
        u32 zero=0;
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.test_spin), &zero, p->snapshot)) ok=false;
        else {
            struct pgwt_fail_spin *spins=(void *)p->snapshot;
            for (int cpu=0; cpu<p->possible; ++cpu) if (spins[cpu].started) {
                spun=true; ++spin_cpus;
                u64 duration=spins[cpu].ended-spins[cpu].started;
                if (duration<minimum_spin) minimum_spin=duration;
                int evidence=probe_spin_evidence((u32)cpu, spins[cpu].started, spins[cpu].ended,
                    p->requests,p->requests_len,p->helper_stamps,p->helper_stamps_len);
                if (evidence < 0) { probe_fail(p,"helper timestamp overtook failure completion"); ok=false; }
                if (!evidence) {
                    fprintf(stderr,"PROBE-REFUSED: no matched request during spin and later helper boundary on CPU %d\n",cpu);
                    refused=true; ok=false;
                }
                fprintf(stderr,"SPIN-EVIDENCE cpu=%d leader_cpu=%d spin_ns=%"PRIu64" start=%"PRIu64" end=%"PRIu64" completed=%"PRIu64" matched=%d\n",
                    cpu,p->main_cpu,duration,spins[cpu].started,spins[cpu].ended,spins[cpu].completed,evidence);
                if (spins[cpu].ended-spins[cpu].started < 1000000) {
                    fprintf(stderr,"PROBE-REFUSED: measured forced-failure spin under 1ms on CPU %d\n",cpu);
                    refused=true; ok=false;
                }
                if (spins[cpu].completed < spins[cpu].ended) ok=false;
            }
        }
        if (!spun) { fprintf(stderr,"PROBE-REFUSED: no forced spin observed\n"); refused=true; ok=false; }
        if (!failed) ok=false;
#else
        fprintf(stderr,"PROBE-REFUSED: production object has no falsifier\n"); refused=true; ok=false;
#endif
    }
    printf("],\"ok\":%s,\"exited_before_attach\":%"PRIu64",\"attach_exit_races\":%"PRIu64",\"attach_failures\":%"PRIu64",\"attempted\":%"PRIu64",\"failures\":%"PRIu64",\"discards\":%"PRIu64",\"init_reserve_failures\":%"PRIu64",\"lost\":%"PRIu64",\"forks\":%"PRIu64",\"pid_reuses\":%"PRIu64",\"attached\":%"PRIu64",\"trace\":%"PRIu64",\"text\":%"PRIu64",\"init_records\":%"PRIu64",\"init_stored\":%"PRIu64",\"spun_at_least_1ms\":%s,\"spin_min_ns\":%"PRIu64",\"spin_cpus\":%d,\"requests\":%zu,\"certified\":false}\n", ok?"true":"false",p->exited_before_attach,p->attach_exit_races,p->attach_failures,total,failed,discarded,init_failures,p->lost,p->forks,p->reuses,p->attached,p->records[PGWT_WIRE_TRACE],p->records[PGWT_WIRE_TEXT],p->init_records,p->init_stored,spun&&!refused?"true":"false",spun?minimum_spin:0,spin_cpus,p->requests_len);
    free(counts); free(fail_counts); free(disc_counts); return p->failed?1:(refused?78:(ok?0:1));
}
/* One load per program and per mode. libbpf stops at the first program the
 * verifier rejects, so a whole-object load cannot say anything about the
 * programs after it; here every program gets its own verdict, with the mode
 * off and on. Nothing is attached. A rejection prints libbpf's verifier log
 * on stderr and makes the exit status nonzero. */
static int probe_verifier_matrix(void)
{
    struct rlimit limit={RLIM_INFINITY,RLIM_INFINITY}; setrlimit(RLIMIT_MEMLOCK,&limit);
    int loads=0, rejected=0;
    for (u32 mode=0; mode<2; ++mode) {
        for (int target=0; ; ++target) {
            struct pg_wait_tracer_bpf *skel=pg_wait_tracer_bpf__open();
            if (!skel) { fprintf(stderr,"PROBE-FAIL: skeleton open\n"); return 1; }
            struct bpf_program *program, *chosen=NULL; int index=0;
            bpf_object__for_each_program(program, skel->obj) {
                bpf_program__set_autoload(program, index == target);
                if (index == target) chosen=program;
                ++index;
            }
            if (!chosen) { pg_wait_tracer_bpf__destroy(skel); break; }
            /* The verifier folds these constants and never walks code they
             * switch off. Turn every gate on, so that the command boundary,
             * both query-text routes, the PG13 edges and CPU accounting are
             * all live code in this load. Nothing is attached, so the
             * addresses are never dereferenced. */
            skel->rodata->target_postmaster_pid=1;
            skel->rodata->my_wait_ptr_addr=4096; skel->rodata->pgproc_wait_offset=684;
            skel->rodata->my_be_entry_addr=4096; skel->rodata->st_query_id_offset=8;
            skel->rodata->debug_query_string_addr=4096;
            skel->rodata->cpu_accounting=access("/sys/kernel/btf/vmlinux", R_OK) == 0;
            skel->rodata->bs_state_running=2; skel->rodata->bs_state_fastpath=4;
            skel->rodata->pg13_query_attr=1; skel->rodata->pg13_synthetic_attr=1;
            skel->rodata->pg13_qd_plannedstmt_off=8; skel->rodata->pg13_ps_queryid_off=8;
            skel->rodata->pg13_qd_sourcetext_off=16;
            skel->rodata->exact_perf_mode=mode;
            skel->rodata->helper_tgid=mode ? (u32)getpid() : 0;
#ifdef PGWT_TRANSPORT_FALSIFIER
            /* Nonzero, so the delayed-failure loop is live code to verify. */
            skel->rodata->test_fail_spin_ns=mode ? 20000000 : 0;
#endif
            if (!skel->rodata->cpu_accounting && chosen == skel->progs.on_sched_switch) {
                /* tp_btf needs kernel BTF; the daemon does not load it either. */
                printf("VERIFIER mode=%u prog=%s SKIPPED-NO-BTF xlated_insns=0\n", mode, bpf_program__name(chosen));
                pg_wait_tracer_bpf__destroy(skel); continue;
            }
            int rc=pg_wait_tracer_bpf__load(skel);
            u32 instructions=0;
            if (!rc) {
                struct bpf_prog_info info; u32 size=sizeof(info);
                memset(&info, 0, sizeof(info));
                if (!bpf_obj_get_info_by_fd(bpf_program__fd(chosen), &info, &size))
                    instructions=info.xlated_prog_len/8;
            }
            printf("VERIFIER mode=%u prog=%s %s xlated_insns=%u\n", mode,
                bpf_program__name(chosen), rc ? "REJECTED" : "ACCEPTED", instructions);
            fflush(stdout);
            ++loads; rejected+=rc != 0;
            pg_wait_tracer_bpf__destroy(skel);
        }
    }
    printf("VERIFIER-MATRIX loads=%d rejected=%d\n", loads, rejected);
    return rejected || !loads ? 1 : 0;
}
static void probe_read_stats(struct probe *p)
{
    static const u32 slots[4] = {PGWT_RING_FAIL_LIFECYCLE_FORK,
        PGWT_RING_FAIL_LIFECYCLE_EXIT, PGWT_RING_FAIL_QUERY_TEXT, PGWT_RING_FAIL_PG13_QUERY_TEXT};
    if (!p->loaded) return;
    u64 *values = calloc((size_t)p->possible, sizeof(*values));
    if (!values) { probe_fail(p, "statistics allocation"); return; }
    p->stats.sites_valid = true;
    for (size_t i=0; i<4; ++i) {
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.ringbuf_site_drops), &slots[i], values)) {
            p->stats.sites_valid = false; probe_fail(p, "statistics site counter read"); break;
        }
        for (int cpu=0; cpu<p->possible; ++cpu) p->stats.site_drops[i] += values[cpu];
    }
    free(values);
}
static void probe_print_stats(const struct probe_stats *stats)
{
    static const char *sites[4] = {"LIFECYCLE_FORK", "LIFECYCLE_EXIT", "QUERY_TEXT", "PG13_QUERY_TEXT"};
    for (size_t i=0; i<4; ++i) {
        fprintf(stderr, "PROBE-STAT: ringbuf_site_drops site=%s value=", sites[i]);
        if (stats->sites_valid) fprintf(stderr, "%"PRIu64, stats->site_drops[i]);
        else fprintf(stderr, "null");
        fputc('\n', stderr);
    }
    fprintf(stderr, "PROBE-STAT: lifecycle_rb max_producer_minus_consumer_bytes=%"PRIu64
        " ring_size_bytes=%"PRIu64" samples=%"PRIu64"\n",
        stats->lifecycle_peak, stats->lifecycle_size, stats->lifecycle_samples);
    fprintf(stderr, "PROBE-STAT: attaches max_between_drains=%"PRIu64" total=%"PRIu64"\n",
        stats->attaches.max, stats->attaches.total);
    fprintf(stderr, "PROBE-STAT: proc_reads max_between_drains=%"PRIu64" total=%"PRIu64"\n",
        stats->proc_reads.max, stats->proc_reads.total);
    fprintf(stderr, "PROBE-STAT: consume max_us=%"PRIu64" total_calls=%"PRIu64"\n",
        stats->consume_max_ns / 1000, stats->consume_calls);
    fprintf(stderr, "PROBE-STAT: lifecycle_rb in_callback_max_producer_minus_consumer_bytes=%"PRIu64"\n",
        stats->lifecycle_callback_peak);
    fprintf(stderr, "PROBE-STAT: teardown max_us=%"PRIu64" total=%"PRIu64"\n",
        stats->teardown_max_ns / 1000, stats->teardown_total);
    fprintf(stderr, "PROBE-STAT: registry max_used=%"PRIu64" size=%u\n",
        stats->registry_max_used, (unsigned)MAX_BACKENDS);
}
static int probe_run(int argc, char **argv, struct probe_stats *stats)
{
    struct probe p={0}; int result=1; u32 pm=0, pg13=0, qd=0, ps=0, source=0, running=2; u64 debug_text=0;
    for (int i=0; i<MAX_BACKENDS; ++i) p.backends[i].boot=p.backends[i].wait=-1;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--verifier-matrix")) {
            if (geteuid()) { fprintf(stderr,"PROBE-REFUSED: root required\n"); return 78; }
            return probe_verifier_matrix();
        }
        if (!strcmp(argv[i], "--self-reuse")) { p.self=true; continue; }
        if (!strcmp(argv[i], "--init-mutant")) { p.init_mutant=true; continue; }
        if (!strcmp(argv[i], "--no-challenge")) { p.no_challenge=true; continue; }
        if (i+1>=argc) return 2;
        const char *key=argv[i++], *value=argv[i];
        u64 n=strtoull(value, NULL, 0);
        if (!strcmp(key,"--pm")) pm=(u32)n;
        else if (!strcmp(key,"--ptr")) p.ptr=n;
        else if (!strcmp(key,"--offset")) p.wait_offset=(u32)n;
        else if (!strcmp(key,"--exe")) p.exe=value;
        else if (!strcmp(key,"--cohort-db")) p.cohort_db=value;
        else if (!strcmp(key,"--activity")) p.activity_offset=n;
        else if (!strcmp(key,"--query")) p.query_offset=n;
        else if (!strcmp(key,"--pg13")) pg13=(u32)n;
        else if (!strcmp(key,"--running")) running=(u32)n;
        else if (!strcmp(key,"--debug-text")) debug_text=n;
        else if (!strcmp(key,"--qd")) qd=(u32)n;
        else if (!strcmp(key,"--ps")) ps=(u32)n;
        else if (!strcmp(key,"--source")) source=(u32)n;
        else if (!strcmp(key,"--spin-ns")) p.spin_ns=n;
        else return 2;
    }
    if (!p.self && (!pm || !p.ptr || !p.exe || !p.cohort_db)) return 2;
    if (geteuid()) { fprintf(stderr,"PROBE-REFUSED: root required\n"); return 78; }
    p.spinning=p.spin_ns != 0;
    if (pthread_mutex_init(&p.request_lock, NULL)) return 1;
    struct rlimit limit={RLIM_INFINITY,RLIM_INFINITY}; setrlimit(RLIMIT_MEMLOCK,&limit);
    p.possible=libbpf_num_possible_cpus(); p.online=(int)sysconf(_SC_NPROCESSORS_ONLN);
    if (p.possible <= 0 || p.possible > MAX_CPUS) { fprintf(stderr,"PROBE-REFUSED: CPU count\n"); return 78; }
    p.delivered=calloc((size_t)p.possible*3,sizeof(u64)); p.last_seq=calloc((size_t)p.possible*3,sizeof(u64));
    p.skipped=calloc((size_t)p.possible*3,sizeof(u64)); p.discard_scratch=calloc((size_t)p.possible,sizeof(u64));
    p.acks=calloc((size_t)p.possible,sizeof(u64));
    p.snapshot=calloc((size_t)p.possible,sizeof(struct pgwt_fail_spin));
    p.pid_lifetimes=calloc(1U<<20,sizeof(*p.pid_lifetimes));
    if (!p.delivered || !p.last_seq || !p.skipped || !p.discard_scratch || !p.acks || !p.snapshot || !p.pid_lifetimes) goto cleanup;
    p.skel=pg_wait_tracer_bpf__open(); if (!p.skel) goto cleanup;
    p.skel->rodata->exact_perf_mode=1; p.skel->rodata->helper_tgid=(u32)getpid();
    p.skel->rodata->target_postmaster_pid=p.self?(u32)getpid():pm;
    p.skel->rodata->my_wait_ptr_addr=p.ptr; p.skel->rodata->pgproc_wait_offset=p.wait_offset;
    p.skel->rodata->bs_state_running=running; p.skel->rodata->bs_state_fastpath=running+2;
    p.skel->rodata->debug_query_string_addr=debug_text;
    p.skel->rodata->pg13_query_attr=pg13; p.skel->rodata->pg13_qd_plannedstmt_off=qd;
    p.skel->rodata->pg13_ps_queryid_off=ps; p.skel->rodata->pg13_qd_sourcetext_off=source;
#ifdef PGWT_TRANSPORT_FALSIFIER
    p.skel->rodata->test_fail_spin_ns=p.spin_ns;
#else
    if (p.spinning) { fprintf(stderr,"PROBE-REFUSED: production object has no falsifier\n"); result=78; goto cleanup; }
#endif
    if (pg_wait_tracer_bpf__load(p.skel)) {
        if (p.spinning) { fprintf(stderr,"PROBE-REFUSED: test spin BPF load/verifier failed; retain verifier log\n"); result=78; }
        goto cleanup;
    }
    p.loaded = true;
    p.stats.lifecycle_size = bpf_map__max_entries(p.skel->maps.lifecycle_rb);
    struct pgwt_exact_config config={.generation=1,.admitting=1}; u32 zero=0;
    if (bpf_map_update_elem(bpf_map__fd(p.skel->maps.exact_config_map), &zero, &config,BPF_ANY)) goto cleanup;
    /* The falsifier arm gets 16 MiB per CPU (262,144 boundary records). On
     * Ubuntu 6.8 this thread drains slowly under connection churn, and with
     * the usual 1 MiB the helpers' boundaries were lost (PERF_RECORD_LOST;
     * one CPU produced 41,781 in the arm). 16 MiB is several times what the
     * challenges can produce in the whole arm even with no drain at all. */
    p.perf=perf_buffer__new(bpf_map__fd(p.skel->maps.perf_events), p.spinning ? 4096 : 256,
        probe_sample, probe_lost, &p, NULL);
    if (libbpf_get_error(p.perf)) { p.perf=NULL; goto cleanup; }
    p.rings=ring_buffer__new(bpf_map__fd(p.skel->maps.event_ringbuf), probe_e, &p, NULL);
    if (!p.perf || !p.rings || ring_buffer__add(p.rings,bpf_map__fd(p.skel->maps.lifecycle_rb),probe_l,&p)) goto cleanup;
    if (probe_link(&p.helper,p.skel->progs.on_transport_helper,"syscalls","sys_enter_getppid")) goto cleanup;
    result=probe_start_helpers(&p); if (result) goto cleanup;
    result=1;
    /* In --self-reuse the leader is the selected fork parent. Create helper
     * threads BEFORE attaching fork/exit so their clone events are excluded. */
    if (probe_link(&p.fork,p.skel->progs.on_fork,"sched","sched_process_fork") ||
        probe_link(&p.exit,p.skel->progs.on_exit,"sched","sched_process_exit")) goto cleanup;
    if (probe_attach_uprobes(&p)) goto cleanup;
    signal(SIGINT,probe_signal); signal(SIGTERM,probe_signal);
    printf("PROBE-READY exact_perf_mode=1 cpus=%d kernel-pid=%d\n",p.online,getpid()); fflush(stdout);
    if (p.self) {
        result=probe_self_reuse(&p); if (result) goto cleanup;
        result=1;
        if (probe_round(&p,true) || probe_drain(&p)<0) goto cleanup;
    } else {
        u64 next=0;
        while (!probe_stop && !p.failed) {
            if (probe_drain(&p) < 0) break;
            probe_service_backends(&p);
            if (probe_now() >= next) { if (probe_round(&p,true)) { p.failed=1; break; } next=probe_now()+(p.spinning?1000000:20000000); }
            struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
            if (poll(&input,1,1)>0 && (input.revents&(POLLIN|POLLHUP))) {
                char line[32]; if (!fgets(line,sizeof(line),stdin) || !strncmp(line,"STOP",4)) break;
            }
        }
    }
    /* Completion round: consume E/L r, request r+1, consume every online
     * CPU's r+1 ack, then (only after shutdown) read attempts. */
    if (probe_round(&p,true)) goto cleanup;
    struct pgwt_transport_ctl fence;
    if (bpf_map_lookup_elem(bpf_map__fd(p.skel->maps.transport_ctl),&zero,&fence)) goto cleanup;
    u64 deadline=probe_now()+5000000000ULL;
    while ((p.boundary_round[1]<fence.round || p.boundary_round[2]<fence.round) && probe_now()<deadline)
        probe_drain(&p);
    if (p.boundary_round[1]<fence.round || p.boundary_round[2]<fence.round)
        probe_fail(&p,"E/L completion candidate not consumed");
    if (probe_round(&p,false)) goto cleanup;
    deadline=probe_now()+5000000000ULL; bool complete=false;
    while (probe_now()<deadline) {
        probe_drain(&p); complete=true;
        struct pgwt_transport_ctl ctl;
        if (bpf_map_lookup_elem(bpf_map__fd(p.skel->maps.transport_ctl),&zero,&ctl)) { complete=false; break; }
        for (int i=0;i<p.helpers_started;++i) if (p.acks[p.helpers[i].cpu]<ctl.round) complete=false;
        if (complete) break;
        sched_yield();
    }
    if (!complete) probe_fail(&p,"completion round missing CPU ack");
    probe_finish(&p); result=probe_reconcile(&p);
    if (!result && p.self && !p.reuses) result=1;
cleanup:
    probe_finish(&p);
    probe_read_stats(&p);
    *stats = p.stats;
    if (p.failed && !result) result=1;
    ring_buffer__free(p.rings); perf_buffer__free(p.perf); pg_wait_tracer_bpf__destroy(p.skel);
    free(p.identities); free(p.delivered); free(p.last_seq); free(p.acks); free(p.snapshot); free(p.helpers); free(p.helper_stamps); free(p.pid_lifetimes);
    free(p.skipped); free(p.discard_scratch);
    free(p.requests);
    return result;
}
/* Keep exit diagnostics independent of argument, refusal and setup paths. */
static int transport_probe(int argc, char **argv)
{
    struct probe_stats stats={0};
    int result = probe_run(argc, argv, &stats);
    probe_print_stats(&stats);
    return result;
}

#endif
