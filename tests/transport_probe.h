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
    struct bpf_link *activity, *query;
};
struct probe_helper {
    pthread_t thread; struct transport_request_channel channel;
    int cpu, error; atomic_int tid; struct probe *probe;
};
struct probe {
    struct pg_wait_tracer_bpf *skel;
    struct ring_buffer *rings; struct perf_buffer *perf;
    struct bpf_link *fork, *exit, *helper;
    struct probe_identity *identities; size_t identities_cap;
    struct probe_pid_cookie *pid_lifetimes;
    struct probe_backend backends[MAX_BACKENDS];
    struct probe_helper *helpers;
    int possible, online, main_cpu, helpers_started, failed;
    u64 *delivered, *last_seq, *acks, *snapshot;
    u64 boundary_w[3]; u32 boundary_round[3];
    struct probe_stamp *helper_stamps, *requests;
    size_t helper_stamps_len, helper_stamps_cap, requests_len, requests_cap;
    u64 records[5], lost, forks, reuses, attached, init_records, init_stored;
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
static int probe_watchpoint(u32 pid, u64 addr, int program, bool enabled)
{
    struct perf_event_attr attr = { .type=PERF_TYPE_BREAKPOINT, .size=sizeof(attr),
        .bp_addr=addr, .bp_type=HW_BREAKPOINT_W, .bp_len=HW_BREAKPOINT_LEN_4,
        .sample_period=1, .disabled=1, .exclude_kernel=1, .exclude_hv=1 };
    int fd = (int)syscall(__NR_perf_event_open, &attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, program) ||
        (enabled && ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))) { close(fd); return -1; }
    return fd;
}
static void probe_close_backend(struct probe_backend *b)
{
    if (b->boot >= 0) close(b->boot);
    if (b->wait >= 0) close(b->wait);
    bpf_link__destroy(b->activity); bpf_link__destroy(b->query);
    memset(b, 0, sizeof(*b)); b->boot = b->wait = -1;
}
static void probe_fail(struct probe *p, const char *why)
{
    fprintf(stderr, "PROBE-FAIL: %s\n", why); p->failed = 1;
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
    ssize_t n=read(fd,title,sizeof(title)-1); close(fd); if (n<=0) return;
    for (ssize_t i=0;i<n;++i) if (!title[i]) title[i]=' ';
    title[n]='\0'; snprintf(needle,sizeof(needle)," %s ",p->cohort_db);
    if (strstr(title,needle)) { b->init_seen=true; ++p->init_stored; }
}
static void probe_try_attach(struct probe *p, struct probe_backend *b)
{
    if (!b->pid || p->self || p->stopping) return;
    probe_observe_stored_init(p,b);
    if (b->wait >= 0) return;
    char name[64]; snprintf(name, sizeof(name), "/proc/%u/mem", b->pid);
    int mem = open(name, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return; /* exited before instrumentation: no A2 coverage claim */
    u64 ptr = 0;
    ssize_t n = pread(mem, &ptr, sizeof(ptr), (off_t)p->ptr);
    if (n != sizeof(ptr) || !ptr) { close(mem); return; }
    u64 addr = ptr + p->wait_offset;
    u32 wait = 0;
    n = pread(mem, &wait, sizeof(wait), (off_t)addr); close(mem);
    if (n != sizeof(wait)) return;
    int fd = probe_watchpoint(b->pid, addr, bpf_program__fd(p->skel->progs.on_watchpoint), false);
    if (fd < 0) return;
    u32 key = b->pid; struct pgwt_pid_state st;
    int map = bpf_map__fd(p->skel->maps.state_map);
    if (bpf_map_lookup_elem(map, &key, &st) || st.cookie != b->cookie) { close(fd); return; }
    st.wait_event_addr = addr; st.last_event = wait; st.last_ts = probe_now(); st.wp_live = 1;
    if (bpf_map_update_elem(map, &key, &st, BPF_ANY) || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0)) {
        close(fd); probe_fail(p, "probe watchpoint seed/enable"); return;
    }
    b->wait = fd; ++p->attached;
    if (b->boot >= 0) { close(b->boot); b->boot = -1; }
}
static void probe_new_backend(struct probe *p, u32 pid, u64 cookie)
{
    if (p->self || p->stopping) return;
    struct probe_backend *slot = NULL;
    for (int i=0; i<MAX_BACKENDS; ++i) {
        if (p->backends[i].pid == pid) probe_close_backend(&p->backends[i]);
        if (!p->backends[i].pid && !slot) slot = &p->backends[i];
    }
    if (!slot) { probe_fail(p, "probe backend registry full"); return; }
    slot->pid = pid; slot->cookie = cookie;
    slot->boot = probe_watchpoint(pid, p->ptr, bpf_program__fd(p->skel->progs.on_bootstrap), true);
    if (p->activity_offset) {
        slot->activity = bpf_program__attach_uprobe(p->skel->progs.on_report_activity, false,
            (pid_t)pid, p->exe, (size_t)p->activity_offset);
        if (libbpf_get_error(slot->activity)) { slot->activity=NULL; /* task may already be gone */ }
    }
    if (p->query_offset) {
        struct bpf_program *program = p->skel->rodata->pg13_query_attr
            ? p->skel->progs.on_executor_start : p->skel->progs.on_report_query_id;
        slot->query = bpf_program__attach_uprobe(program, false, (pid_t)pid, p->exe, (size_t)p->query_offset);
        if (libbpf_get_error(slot->query)) slot->query=NULL;
    }
    probe_try_attach(p, slot);
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
    if (hdr.seq != p->last_seq[index]+1 && !p->spinning) probe_fail(p, "attempt sequence gap/reorder");
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
            ++p->init_records;
            for (int i=0; i<MAX_BACKENDS; ++i)
                if (p->backends[i].pid == pid && p->backends[i].cookie == hdr.cookie) probe_try_attach(p, &p->backends[i]);
        }
        if (ev.type == PGWT_LIFECYCLE_EXIT) for (int i=0; i<MAX_BACKENDS; ++i)
            if (p->backends[i].pid == pid && p->backends[i].cookie == hdr.cookie) probe_close_backend(&p->backends[i]);
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
static int probe_l(void *ctx, void *data, size_t size) { return probe_record(ctx, 2, data, size, -1); }
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
    while (transport_request_wait(&h->channel)) {
        if (!h->error) {
            u64 until=probe_now() + (h->probe->spinning ? 20000000 : 0);
            do { syscall(SYS_getppid); } while (h->probe->spinning && probe_now()<until);
        }
    }
    return NULL;
}
static int probe_drain(struct probe *p)
{
    int a = ring_buffer__consume(p->rings), b = perf_buffer__consume(p->perf);
    if (a < 0 || b < 0) { probe_fail(p, "buffer consume"); return -1; }
    return a+b;
}
static int probe_round(struct probe *p, bool el)
{
    u32 zero=0; struct pgwt_transport_ctl ctl;
    int fd=bpf_map__fd(p->skel->maps.transport_ctl);
    if (bpf_map_lookup_elem(fd, &zero, &ctl)) return -1;
    ++ctl.round; if (el) ++ctl.el_request;
    if (bpf_map_update_elem(fd, &zero, &ctl, BPF_ANY)) return -1;
    for (int i=0; i<p->helpers_started; ++i) {
        transport_request_signal(&p->helpers[i].channel);
        if (p->spinning) {
            if (p->requests_len == p->requests_cap) {
                size_t cap=p->requests_cap ? p->requests_cap*2 : 1024;
                void *next=realloc(p->requests,cap*sizeof(*p->requests));
                if (!next) return -1;
                p->requests=next; p->requests_cap=cap;
            }
            /* Stamp after signal: this request was issued by this time. */
            p->requests[p->requests_len++]=(struct probe_stamp){
                .cpu=(u32)p->helpers[i].cpu, .round=ctl.round, .stamp=probe_now()};
        }
    }
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
    int fd=probe_watchpoint(key, (u64)(uintptr_t)&probe_wait, bpf_program__fd(p->skel->progs.on_watchpoint), false);
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
    return 0;
}
static void probe_finish(struct probe *p)
{
    p->stopping=true;
    for (int i=0; i<p->helpers_started; ++i) transport_request_stop(&p->helpers[i].channel);
    for (int i=0; i<p->helpers_started; ++i) { pthread_join(p->helpers[i].thread, NULL); transport_request_destroy(&p->helpers[i].channel); }
    p->helpers_started=0;
    bpf_link__destroy(p->helper); p->helper=NULL;
    bpf_link__destroy(p->fork); p->fork=NULL;
    for (int i=0; i<MAX_BACKENDS; ++i) probe_close_backend(&p->backends[i]);
    bpf_link__destroy(p->exit); p->exit=NULL;
    if (p->rings && p->perf) while (probe_drain(p)>0) {}
}
static int probe_reconcile(struct probe *p)
{
    u64 *counts=calloc((size_t)p->possible, sizeof(u64)); if (!counts) return -1;
    bool ok=!p->failed, refused=false; u64 total=0, failed=0;
    u64 *fail_counts=calloc((size_t)p->possible,sizeof(u64)); if (!fail_counts) { free(counts); return 1; }
    printf("{\"cells\":[");
    for (u32 dest=0; dest<3; ++dest) {
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_attempts), &dest, counts)) { ok=false; break; }
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_failures), &dest, fail_counts)) { ok=false; break; }
        for (int cpu=0; cpu<p->possible; ++cpu) {
            size_t index=(size_t)dest*p->possible+cpu;
            u64 delivered=p->delivered[index]; total+=counts[cpu];
            u64 expected_missing=p->spinning ? fail_counts[cpu] : 0;
            if (counts[cpu] != delivered+expected_missing || p->last_seq[index]>counts[cpu]) ok=false;
            if (!p->spinning && counts[cpu] != p->last_seq[index]) ok=false;
            printf("%s{\"dest\":%u,\"cpu\":%d,\"attempted\":%"PRIu64",\"delivered\":%"PRIu64",\"failed\":%"PRIu64",\"last_seq\":%"PRIu64"}", dest||cpu?",":"", dest, cpu, counts[cpu], delivered, fail_counts[cpu], p->last_seq[index]);
        }
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.transport_failures), &dest, counts)) ok=false;
        else for (int cpu=0; cpu<p->possible; ++cpu) failed+=counts[cpu];
    }
    for (size_t i=1; i<p->identities_cap; ++i) if (p->identities[i].records && !p->spinning) {
        struct probe_identity *id=&p->identities[i];
        if (!id->fork_seen || (id->min_ts && id->min_ts<id->fork_ts) ||
            (id->exit_seen && id->max_ts>id->exit_ts)) {
            probe_fail(p,"record cookie does not match its FORK-to-EXIT lifetime"); ok=false;
        }
    }
    if (!p->records[PGWT_WIRE_TRACE] || !p->forks || p->lost || (!p->spinning && failed)) ok=false;
    bool spun=false; u64 minimum_spin=~(u64)0;
    if (p->spinning) {
#ifdef PGWT_TRANSPORT_FALSIFIER
        u32 zero=0;
        if (bpf_map_lookup_elem(bpf_map__fd(p->skel->maps.test_spin), &zero, p->snapshot)) ok=false;
        else {
            struct pgwt_fail_spin *spins=(void *)p->snapshot;
            for (int cpu=0; cpu<p->possible; ++cpu) if (spins[cpu].started) {
                spun=true;
                u64 duration=spins[cpu].ended-spins[cpu].started;
                if (duration<minimum_spin) minimum_spin=duration;
                int evidence=probe_spin_evidence((u32)cpu, spins[cpu].started, spins[cpu].ended,
                    p->requests,p->requests_len,p->helper_stamps,p->helper_stamps_len);
                if (evidence < 0) { probe_fail(p,"helper timestamp overtook failure completion"); ok=false; }
                if (!evidence) {
                    fprintf(stderr,"PROBE-REFUSED: no matched request during spin and later helper boundary on CPU %d\n",cpu);
                    refused=true; ok=false;
                }
                fprintf(stderr,"SPIN-EVIDENCE cpu=%d start=%"PRIu64" end=%"PRIu64" completed=%"PRIu64" matched=%d\n",
                    cpu,spins[cpu].started,spins[cpu].ended,spins[cpu].completed,evidence);
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
    printf("],\"ok\":%s,\"attempted\":%"PRIu64",\"failures\":%"PRIu64",\"lost\":%"PRIu64",\"forks\":%"PRIu64",\"pid_reuses\":%"PRIu64",\"attached\":%"PRIu64",\"trace\":%"PRIu64",\"init_records\":%"PRIu64",\"init_stored\":%"PRIu64",\"spun_at_least_1ms\":%s,\"spin_min_ns\":%"PRIu64",\"certified\":false}\n", ok?"true":"false",total,failed,p->lost,p->forks,p->reuses,p->attached,p->records[1],p->init_records,p->init_stored,spun&&!refused?"true":"false",spun?minimum_spin:0);
    free(counts); free(fail_counts); return p->failed?1:(refused?78:(ok?0:1));
}
static int transport_probe(int argc, char **argv)
{
    struct probe p={0}; int result=1; u32 pm=0, pg13=0, qd=0, ps=0, source=0, running=2; u64 debug_text=0;
    for (int i=0; i<MAX_BACKENDS; ++i) p.backends[i].boot=p.backends[i].wait=-1;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--self-reuse")) { p.self=true; continue; }
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
    struct rlimit limit={RLIM_INFINITY,RLIM_INFINITY}; setrlimit(RLIMIT_MEMLOCK,&limit);
    p.possible=libbpf_num_possible_cpus(); p.online=(int)sysconf(_SC_NPROCESSORS_ONLN);
    if (p.possible <= 0 || p.possible > MAX_CPUS) { fprintf(stderr,"PROBE-REFUSED: CPU count\n"); return 78; }
    p.delivered=calloc((size_t)p.possible*3,sizeof(u64)); p.last_seq=calloc((size_t)p.possible*3,sizeof(u64));
    p.acks=calloc((size_t)p.possible,sizeof(u64));
    p.snapshot=calloc((size_t)p.possible,sizeof(struct pgwt_fail_spin));
    p.pid_lifetimes=calloc(1U<<20,sizeof(*p.pid_lifetimes));
    if (!p.delivered || !p.last_seq || !p.acks || !p.snapshot || !p.pid_lifetimes) goto cleanup;
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
    struct pgwt_exact_config config={.generation=1,.admitting=1}; u32 zero=0;
    if (bpf_map_update_elem(bpf_map__fd(p.skel->maps.exact_config_map), &zero, &config,BPF_ANY)) goto cleanup;
    p.perf=perf_buffer__new(bpf_map__fd(p.skel->maps.perf_events), 256, probe_sample, probe_lost, &p, NULL);
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
    signal(SIGINT,probe_signal); signal(SIGTERM,probe_signal);
    printf("PROBE-READY exact_perf_mode=1 cpus=%d kernel-pid=%d\n",p.online,getpid()); fflush(stdout);
    if (p.self) {
        result=probe_self_reuse(&p); if (result) goto cleanup;
        result=1;
        if (probe_round(&p,true) || probe_drain(&p)<0) goto cleanup;
    } else {
        u64 next=0;
        while (!probe_stop && !p.failed) {
            probe_drain(&p);
            for (int i=0;i<MAX_BACKENDS;++i) probe_try_attach(&p,&p.backends[i]);
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
    ring_buffer__free(p.rings); perf_buffer__free(p.perf); pg_wait_tracer_bpf__destroy(p.skel);
    free(p.identities); free(p.delivered); free(p.last_seq); free(p.acks); free(p.snapshot); free(p.helpers); free(p.helper_stamps); free(p.pid_lifetimes);
    free(p.requests);
    return result;
}
#endif
