#define _GNU_SOURCE
#include "reader.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

struct writer {
    _Alignas(64) volatile uint64_t watched;
    pthread_t thread;
    pid_t tid;
    int cpu, error, fd;
    uint64_t iterations;
};
struct cpu_result {
    uint64_t delivered, last_seq, gaps, reorders;
    uint32_t expected_tid;
};
struct experiment {
    enum transport_kind kind;
    const char *name, *object_path, *stage;
    int possible, available, threads, started;
    int *cpu_ids, *ring_fds;
    size_t *ring_sizes, *perf_sizes;
    size_t bytes, page;
    double duration, elapsed, max_gap_ms;
    struct writer *writers;
    struct cpu_result *results;
    struct transport_counts *counts;
    struct bpf_object *object;
    struct ring_buffer *ring;
    struct perf_buffer **perf;
    uint64_t lost, lost_callbacks, bad_records;
    int error, saved_errno;
    bool accepted;
};
static atomic_int ready, start_writers, stop_writers;
static atomic_int interrupted;
static double writer_deadline;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal handler needs lock-free int atomics");
static void on_signal(int sig)
{
    (void)sig;
    atomic_store_explicit(&interrupted, 1, memory_order_relaxed);
    atomic_store_explicit(&stop_writers, 1, memory_order_relaxed);
}
static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void *write_loop(void *arg)
{
    struct writer *w = arg;
    cpu_set_t *mask = CPU_ALLOC((size_t)w->cpu + 1);
    size_t size = CPU_ALLOC_SIZE((size_t)w->cpu + 1);
    if (!mask) w->error = ENOMEM;
    else {
        CPU_ZERO_S(size, mask);
        CPU_SET_S(w->cpu, size, mask);
        w->error = pthread_setaffinity_np(pthread_self(), size, mask);
        CPU_FREE(mask);
    }
    w->tid = (pid_t)syscall(SYS_gettid);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&start_writers, memory_order_acquire) &&
           !atomic_load_explicit(&stop_writers, memory_order_relaxed)) sched_yield();
    if (w->error) return NULL;
    uint64_t n = 0;
    while (!atomic_load_explicit(&stop_writers, memory_order_relaxed)) {
        /* A saturated libbpf drain can keep consuming indefinitely. Writers
         * enforce time independently; amortize the vDSO clock read over 256
         * stores. The start gate publishes the common deadline. */
        if (!(n & 255) && now_seconds() >= writer_deadline) break;
        w->watched = ++n;
    }
    w->iterations = n;
    return NULL;
}

static int open_breakpoint(struct writer *w, int program)
{
    /* Same disabled-first, per-TID recipe as src/perf_event.c, length 8. */
    struct perf_event_attr attr = {0};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_W;
    attr.bp_addr = (uint64_t)(uintptr_t)&w->watched;
    attr.bp_len = HW_BREAKPOINT_LEN_8;
    attr.sample_period = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    int fd = (int)syscall(__NR_perf_event_open, &attr, w->tid, -1, -1,
                          PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, program)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
static int receive_record(void *ctx, void *data, size_t size)
{
    struct experiment *e = ctx;
    struct transport_record r;
    if (size != sizeof(r)) { ++e->bad_records; return 0; }
    memcpy(&r, data, sizeof(r));
    if (r.cpu >= (uint32_t)e->possible) { ++e->bad_records; return 0; }
    struct cpu_result *c = &e->results[r.cpu];
    if (!transport_record_valid(&r, c->expected_tid)) {
        ++e->bad_records;
        return 0;
    }
    ++c->delivered;
    if (r.seq <= c->last_seq) ++c->reorders;
    else {
        c->gaps += r.seq - c->last_seq - 1;
        c->last_seq = r.seq;
    }
    return 0;
}
static void receive_perf(void *ctx, int cpu, const void *sample, size_t size)
{
    struct experiment *e = ctx;
    struct transport_record r;
    if (!transport_decode_perf(&r, sample, size)) { ++e->bad_records; return; }
    if (r.cpu != (uint32_t)cpu) { ++e->bad_records; return; }
    (void)receive_record(ctx, &r, sizeof(r));
}
/* Raw API allows selecting the CPU and an explicit byte watermark. */
static enum bpf_perf_event_ret receive_raw(void *ctx, int cpu,
                                           struct perf_event_header *event)
{
    struct experiment *e = ctx;
    const unsigned char *payload = (const unsigned char *)(event + 1);
    if (event->type == PERF_RECORD_SAMPLE) {
        if (event->size < sizeof(*event)) goto malformed;
        receive_perf(ctx, cpu, payload, event->size - sizeof(*event));
    } else if (event->type == PERF_RECORD_LOST) {
        uint64_t lost;
        if (event->size < sizeof(*event) + 2 * sizeof(uint64_t)) goto malformed;
        memcpy(&lost, payload + sizeof(uint64_t), sizeof(lost));
        e->lost += lost;
        ++e->lost_callbacks;
    } else goto malformed;
    return LIBBPF_PERF_EVENT_CONT;
malformed:
    ++e->bad_records;
    return LIBBPF_PERF_EVENT_CONT;
}

/* Sum exactly to the shared ring's power-of-two budget, even on machines
 * whose possible/allowed CPU counts are not powers of two. Every individual
 * buffer remains a power-of-two multiple of a page. This distributes the
 * remainder deterministically, lowest CPU first. Ringbuf map metadata does
 * not require equal max_entries (kernel/bpf/map_in_map.c). */
static int split_budget(size_t total, size_t page, int n, size_t *sizes)
{
    if (n <= 0 || (size_t)n > total / page) return -1;
    size_t left = total - (size_t)n * page;
    for (int i = 0; i < n; ++i) sizes[i] = page;
    while (left) {
        int smallest = -1;
        for (int i = 0; i < n; ++i)
            if (sizes[i] <= left && (smallest < 0 || sizes[i] < sizes[smallest])) smallest = i;
        if (smallest < 0) return -1;
        left -= sizes[smallest];
        sizes[smallest] *= 2;
    }
    return 0;
}
static int configure_object(struct experiment *e)
{
    e->stage = "object_open";
    e->object = bpf_object__open_file(e->object_path, NULL);
    long open_error = libbpf_get_error(e->object);
    if (!e->object || open_error) {
        e->object = NULL;
        return open_error ? (int)open_error : -errno;
    }
    struct bpf_map *shared = bpf_object__find_map_by_name(e->object, "shared_ring");
    struct bpf_map *outer = bpf_object__find_map_by_name(e->object, "cpu_rings");
    struct bpf_map *perf = bpf_object__find_map_by_name(e->object, "perf_events");
    if (!shared || !outer || !perf) return -ENOENT;
    int rc;
    if ((rc = bpf_map__set_autocreate(shared, e->kind == TRANSPORT_SHARED)) ||
        (rc = bpf_map__set_autocreate(outer, e->kind == TRANSPORT_PERCPU_RING)) ||
        (rc = bpf_map__set_autocreate(perf, e->kind == TRANSPORT_PERF))) return rc;
    struct bpf_map *map;
    bool configured = false;
    bpf_object__for_each_map(map, e->object) {
        if (!bpf_map__is_internal(map) || !strstr(bpf_map__name(map), ".rodata")) continue;
        unsigned int mode = (unsigned int)e->kind;
        if (bpf_map__value_size(map) != sizeof(mode)) return -EINVAL;
        if ((rc = bpf_map__set_initial_value(map, &mode, sizeof(mode)))) return rc;
        configured = true;
    }
    if (!configured) return -ENOENT;
    if (e->kind == TRANSPORT_SHARED) {
        if ((rc = bpf_map__set_max_entries(shared, (__u32)e->bytes))) return rc;
    } else if (e->kind == TRANSPORT_PERCPU_RING) {
        e->stage = "inner_map_create";
        for (int cpu = 0; cpu < e->possible; ++cpu) {
            e->ring_fds[cpu] = bpf_map_create(BPF_MAP_TYPE_RINGBUF, "nmi_cpu_ring", 0, 0,
                                             (__u32)e->ring_sizes[cpu], NULL);
            if (e->ring_fds[cpu] < 0) return -errno;
        }
        if ((rc = bpf_map__set_max_entries(outer, (__u32)e->possible)) ||
            (rc = bpf_map__set_inner_map_fd(outer, e->ring_fds[0]))) return rc;
    } else {
        if ((rc = bpf_map__set_max_entries(perf, (__u32)e->possible))) return rc;
    }
    e->stage = "object_load";
    if ((rc = bpf_object__load(e->object))) return rc;
    if (e->kind == TRANSPORT_PERCPU_RING) {
        e->stage = "inner_map_install";
        for (int cpu = 0; cpu < e->possible; ++cpu)
            if (bpf_map_update_elem(bpf_map__fd(outer), &cpu, &e->ring_fds[cpu], BPF_ANY))
                return -errno;
    }
    e->accepted = true;
    e->stage = "consumer_create";
    if (e->kind == TRANSPORT_PERF) {
        /* One libbpf consumer per allowed online CPU permits exact total
         * sizing with heterogeneous capacities; all share the same map. */
        for (int i = 0; i < e->available; ++i) {
            struct perf_buffer_raw_opts opts = {
                .sz = sizeof(opts), .cpu_cnt = 1,
                .cpus = &e->cpu_ids[i], .map_keys = &e->cpu_ids[i]
            };
            struct perf_event_attr attr = {0};
            attr.size = sizeof(attr);
            attr.type = PERF_TYPE_SOFTWARE;
            attr.config = PERF_COUNT_SW_BPF_OUTPUT;
            attr.sample_type = PERF_SAMPLE_RAW;
            attr.sample_period = 1;
            attr.watermark = 1;
            size_t watermark = e->perf_sizes[i] / 8;
            attr.wakeup_watermark = (__u32)(watermark < 65536 ? watermark : 65536);
            e->perf[i] = perf_buffer__new_raw(bpf_map__fd(perf), e->perf_sizes[i] / e->page,
                                              &attr, receive_raw, e, &opts);
            long err = libbpf_get_error(e->perf[i]);
            if (!e->perf[i] || err) {
                e->perf[i] = NULL;
                return err ? (int)err : -errno;
            }
        }
    } else {
        int n = e->kind == TRANSPORT_SHARED ? 1 : e->possible;
        for (int i = 0; i < n; ++i) {
            int fd = e->kind == TRANSPORT_SHARED ? bpf_map__fd(shared) : e->ring_fds[i];
            if (!e->ring) {
                e->ring = ring_buffer__new(fd, receive_record, e, NULL);
                long err = libbpf_get_error(e->ring);
                if (!e->ring || err) { e->ring = NULL; return err ? (int)err : -errno; }
            } else if ((rc = ring_buffer__add(e->ring, fd, receive_record, e))) return rc;
        }
    }
    return 0;
}
static int drain(struct experiment *e, bool wait)
{
    int rc, total = 0;
    if (e->kind != TRANSPORT_PERF) {
        rc = ring_buffer__poll(e->ring, wait ? 10 : 0);
        if (rc < 0 && rc != -EINTR) return rc;
        if (rc > 0) total += rc;
        /* NO_WAKEUP matches the daemon: an epoll timeout is NOT a drain. */
        rc = ring_buffer__consume(e->ring);
        if (rc < 0 && rc != -EINTR) return rc;
        if (rc > 0) total += rc;
    } else {
        for (int i = 0; i < e->available; ++i) {
            rc = perf_buffer__poll(e->perf[i], i == 0 && wait ? 10 : 0);
            if (rc < 0 && rc != -EINTR) return rc;
            if (rc > 0) total += rc;
            rc = perf_buffer__consume(e->perf[i]);
            if (rc < 0 && rc != -EINTR) return rc;
            if (rc > 0) total += rc;
        }
    }
    return total;
}
static int run(struct experiment *e)
{
    e->stage = "writers_start";
    for (int i = 0; i < e->threads; ++i) {
        struct writer *w = &e->writers[i];
        w->cpu = e->cpu_ids[i];
        int rc = pthread_create(&w->thread, NULL, write_loop, w);
        if (rc) return -rc;
        ++e->started;
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) < e->threads) sched_yield();
    struct bpf_program *program = bpf_object__find_program_by_name(e->object, "on_write");
    if (!program) return -ENOENT;
    for (int i = 0; i < e->threads; ++i) {
        struct writer *w = &e->writers[i];
        if (w->error) return -w->error;
        e->results[w->cpu].expected_tid = (uint32_t)w->tid;
        e->stage = "breakpoint_attach";
        w->fd = open_breakpoint(w, bpf_program__fd(program));
        if (w->fd < 0) return -errno;
        if (ioctl(w->fd, PERF_EVENT_IOC_ENABLE, 0)) return -errno;
    }
    e->stage = "measurement";
    double began = now_seconds(), last = began;
    writer_deadline = began + e->duration;
    atomic_store_explicit(&start_writers, 1, memory_order_release);
    while (!atomic_load_explicit(&interrupted, memory_order_relaxed) &&
           now_seconds() - began < e->duration) {
        int rc = drain(e, true);
        if (rc < 0) return rc;
        double current = now_seconds();
        double gap = (current - last) * 1000;
        if (gap > e->max_gap_ms) e->max_gap_ms = gap;
        last = current;
    }
    atomic_store_explicit(&stop_writers, 1, memory_order_relaxed);
    for (int i = 0; i < e->started; ++i) pthread_join(e->writers[i].thread, NULL);
    e->started = 0;
    e->elapsed = now_seconds() - began;
    e->stage = "quiescence";
    for (int i = 0; i < e->threads; ++i)
        if (ioctl(e->writers[i].fd, PERF_EVENT_IOC_DISABLE, 0)) return -errno;
    /* Joined writers can never produce again. Drain once more even without
     * readiness; LOST can be absent after the final failed helper attempt. */
    int rc = drain(e, false);
    if (rc < 0) return rc;
    double final_gap = (now_seconds() - last) * 1000;
    if (final_gap > e->max_gap_ms) e->max_gap_ms = final_gap;
    uint32_t zero = 0;
    int fd = bpf_object__find_map_fd_by_name(e->object, "counts");
    if (fd < 0) return fd;
    if (bpf_map_lookup_elem(fd, &zero, e->counts)) return -errno;
    for (int cpu = 0; cpu < e->possible; ++cpu) {
        struct cpu_result *c = &e->results[cpu];
        if (e->counts[cpu].attempts > c->last_seq)
            c->gaps += e->counts[cpu].attempts - c->last_seq;
    }
    e->stage = "complete";
    return atomic_load_explicit(&interrupted, memory_order_relaxed) ? -EINTR : 0;
}
static void json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void report(struct experiment *e)
{
    struct utsname uts;
    const char *kernel = uname(&uts) ? "unknown" : uts.release;
    struct transport_counts sum = {0};
    uint64_t hits = 0, delivered = 0, gaps = 0, reorders = 0;
    for (int cpu = 0; cpu < e->possible; ++cpu) {
        struct transport_counts *c = &e->counts[cpu];
        sum.attempts += c->attempts; sum.eagain += c->eagain;
        sum.enospc += c->enospc; sum.other += c->other; sum.reserve_null += c->reserve_null;
        delivered += e->results[cpu].delivered;
        gaps += e->results[cpu].gaps; reorders += e->results[cpu].reorders;
    }
    for (int i = 0; i < e->threads; ++i) hits += e->writers[i].iterations;
    printf("{\"transport\":"); json_string(e->name);
    printf(",\"kernel\":"); json_string(kernel);
    printf(",\"cpus\":%d,\"possible_cpus\":%d,\"threads\":%d,\"duration\":%.6f,"
           "\"elapsed\":%.6f,\"buffer_bytes_total\":%zu,\"hits\":%" PRIu64
           ",\"attempts\":%" PRIu64 ",\"failures\":{\"EAGAIN\":%" PRIu64
           ",\"ENOSPC\":%" PRIu64 ",\"other\":%" PRIu64 "},\"reserve_null\":%" PRIu64
           ",\"delivered\":%" PRIu64 ",\"lost_notifications\":%" PRIu64
           ",\"lost_callbacks\":%" PRIu64 ",\"seq_gaps\":%" PRIu64
           ",\"seq_reorders\":%" PRIu64 ",\"bad_records\":%" PRIu64
           ",\"max_drain_gap_ms\":%.6f,\"accepted\":%s,\"stage\":",
           e->available, e->possible, e->threads, e->duration, e->elapsed, e->bytes,
           hits, sum.attempts, sum.eagain, sum.enospc, sum.other, sum.reserve_null,
           delivered, e->lost, e->lost_callbacks, gaps, reorders, e->bad_records,
           e->max_gap_ms, e->accepted ? "true" : "false");
    json_string(e->stage);
    printf(",\"libbpf_error\":%d,\"errno\":%d,\"error\":", e->error, e->saved_errno);
    json_string(e->error ? strerror(-e->error) : "");
    printf(",\"buffer_bytes\":[");
    int n = e->kind == TRANSPORT_SHARED ? 1 : e->kind == TRANSPORT_PERCPU_RING ? e->possible : e->available;
    for (int i = 0; i < n; ++i)
        printf("%s%zu", i ? "," : "", e->kind == TRANSPORT_SHARED ? e->bytes :
               e->kind == TRANSPORT_PERCPU_RING ? e->ring_sizes[i] : e->perf_sizes[i]);
    printf("],\"buffer_cpus\":[");
    for (int i = 0; i < n; ++i)
        printf("%s%d", i ? "," : "", e->kind == TRANSPORT_SHARED ? -1 :
               e->kind == TRANSPORT_PERCPU_RING ? i : e->cpu_ids[i]);
    printf("],\"per_cpu\":[");
    for (int i = 0; i < e->threads; ++i) {
        int cpu = e->writers[i].cpu;
        struct cpu_result *c = &e->results[cpu];
        printf("%s{\"cpu\":%d,\"tid\":%d,\"hits\":%" PRIu64
               ",\"attempts\":%" PRIu64 ",\"delivered\":%" PRIu64
               ",\"EAGAIN\":%" PRIu64 ",\"ENOSPC\":%" PRIu64
               ",\"other\":%" PRIu64 ",\"reserve_null\":%" PRIu64
               ",\"seq_gaps\":%" PRIu64 ",\"seq_reorders\":%" PRIu64 "}",
               i ? "," : "", cpu, e->writers[i].tid, e->writers[i].iterations,
               e->counts[cpu].attempts, c->delivered, e->counts[cpu].eagain,
               e->counts[cpu].enospc, e->counts[cpu].other, e->counts[cpu].reserve_null,
               c->gaps, c->reorders);
    }
    puts("]}");
}
static void cleanup(struct experiment *e)
{
    atomic_store(&stop_writers, 1);
    for (int i = 0; i < e->started; ++i) pthread_join(e->writers[i].thread, NULL);
    if (e->writers)
        for (int i = 0; i < e->threads; ++i) if (e->writers[i].fd >= 0) close(e->writers[i].fd);
    if (e->ring) ring_buffer__free(e->ring);
    if (e->perf)
        for (int i = 0; i < e->available; ++i) if (e->perf[i]) perf_buffer__free(e->perf[i]);
    if (e->object) bpf_object__close(e->object);
    if (e->ring_fds)
        for (int i = 0; i < e->possible; ++i) if (e->ring_fds[i] >= 0) close(e->ring_fds[i]);
    free(e->cpu_ids); free(e->writers); free(e->results); free(e->counts);
    free(e->ring_fds); free(e->ring_sizes); free(e->perf_sizes); free(e->perf);
}
static int parse_size(const char *s, size_t *value)
{
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *s == '-' || !*s || *end || !n || n > SIZE_MAX) return -1;
    *value = (size_t)n;
    return 0;
}
static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --transport shared|percpu-ring|perf [--threads N] "
            "[--duration SECONDS] [--bytes TOTAL] [--object transport.bpf.o]\n", program);
}
int main(int argc, char **argv)
{
    struct experiment e = {
        .name = "shared", .object_path = "transport.bpf.o", .stage = "setup",
        .bytes = 64 * 1024 * 1024, .duration = 30
    };
    static const struct option options[] = {
        {"transport", required_argument, NULL, 't'}, {"threads", required_argument, NULL, 'n'},
        {"duration", required_argument, NULL, 'd'}, {"bytes", required_argument, NULL, 'b'},
        {"object", required_argument, NULL, 'o'}, {"help", no_argument, NULL, 'h'}, {0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "t:n:d:b:o:h", options, NULL)) != -1) {
        size_t n;
        switch (opt) {
        case 't':
            if (!strcmp(optarg, "shared")) e.kind = TRANSPORT_SHARED;
            else if (!strcmp(optarg, "percpu-ring")) e.kind = TRANSPORT_PERCPU_RING;
            else if (!strcmp(optarg, "perf")) e.kind = TRANSPORT_PERF;
            else goto invalid;
            e.name = optarg;
            break;
        case 'n': if (parse_size(optarg, &n) || n > INT_MAX) goto invalid; e.threads = (int)n; break;
        case 'd': if (parse_size(optarg, &n) || n > 86400) goto invalid; e.duration = (double)n; break;
        case 'b': if (parse_size(optarg, &e.bytes)) goto invalid; break;
        case 'o': e.object_path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default: goto invalid;
        }
    }
    if (optind != argc) goto invalid;
    e.possible = libbpf_num_possible_cpus();
    long page = sysconf(_SC_PAGESIZE);
    if (e.possible <= 0 || page <= 0) { fprintf(stderr, "cannot discover CPUs/page size\n"); return 1; }
    e.page = (size_t)page;
    /* Reject rather than silently round a caller's total budget. */
    if (e.bytes > UINT32_MAX || e.bytes < e.page || (e.bytes & (e.bytes - 1))) goto invalid;
    cpu_set_t *mask = CPU_ALLOC((size_t)e.possible);
    size_t mask_size = CPU_ALLOC_SIZE((size_t)e.possible);
    e.cpu_ids = calloc((size_t)e.possible, sizeof(*e.cpu_ids));
    if (!mask || !e.cpu_ids) { CPU_FREE(mask); cleanup(&e); return 1; }
    CPU_ZERO_S(mask_size, mask);
    if (sched_getaffinity(0, mask_size, mask)) {
        perror("sched_getaffinity"); CPU_FREE(mask); cleanup(&e); return 1;
    }
    for (int cpu = 0; cpu < e.possible; ++cpu)
        if (CPU_ISSET_S(cpu, mask_size, mask)) e.cpu_ids[e.available++] = cpu;
    CPU_FREE(mask);
    if (!e.threads) e.threads = e.available;
    if (!e.available || e.threads > e.available) { cleanup(&e); goto invalid; }
    size_t writers_size = (size_t)e.threads * sizeof(*e.writers);
    e.writers = aligned_alloc(64, writers_size);
    e.results = calloc((size_t)e.possible, sizeof(*e.results));
    e.counts = calloc((size_t)e.possible, sizeof(*e.counts));
    e.ring_fds = malloc((size_t)e.possible * sizeof(*e.ring_fds));
    e.ring_sizes = calloc((size_t)e.possible, sizeof(*e.ring_sizes));
    e.perf_sizes = calloc((size_t)e.available, sizeof(*e.perf_sizes));
    e.perf = calloc((size_t)e.available, sizeof(*e.perf));
    if (e.ring_fds) for (int i = 0; i < e.possible; ++i) e.ring_fds[i] = -1;
    if (e.writers) {
        memset(e.writers, 0, writers_size);
        for (int i = 0; i < e.threads; ++i) { e.writers[i].fd = -1; e.writers[i].cpu = e.cpu_ids[i]; }
    }
    if (!e.writers || !e.results || !e.counts || !e.ring_fds || !e.ring_sizes || !e.perf_sizes || !e.perf) {
        fprintf(stderr, "allocation failure\n"); cleanup(&e); return 1;
    }
    if (split_budget(e.bytes, e.page, e.possible, e.ring_sizes) ||
        split_budget(e.bytes, e.page, e.available, e.perf_sizes)) {
        fprintf(stderr, "budget must provide at least one page per possible CPU\n"); cleanup(&e); return 2;
    }
    struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &limit)) perror("setrlimit MEMLOCK (continuing)");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    errno = 0;
    e.error = configure_object(&e);
    if (!e.error) e.error = run(&e);
    e.saved_errno = errno;
    if (e.error) fprintf(stderr, "%s: libbpf/operation error=%d errno=%d (%s)\n",
                         e.stage, e.error, e.saved_errno, strerror(-e.error));
    /* On early failures stop and join before reading writer-owned data. */
    atomic_store(&stop_writers, 1);
    for (int i = 0; i < e.started; ++i) pthread_join(e.writers[i].thread, NULL);
    e.started = 0;
    report(&e);
    int status = e.error ? (e.accepted ? 1 : 3) : 0;
    cleanup(&e);
    return status;
invalid:
    usage(argv[0]);
    return 2;
}
