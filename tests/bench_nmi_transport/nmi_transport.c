#define _GNU_SOURCE
#include "reader.h"
#include "cadence.h"
#include "cpu_accounting.h"
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

struct experiment;
struct writer {
    _Alignas(64) volatile uint64_t watched;
    pthread_t thread;
    pid_t tid;
    int cpu, error, fd;
    uint64_t iterations, migrations;
    struct experiment *experiment;
};
struct boundary_helper {
    pthread_t thread;
    pid_t tid;
    int cpu, error;
    uint64_t cpu_ns, wakeups, syscalls;
    struct experiment *experiment;
};
struct experiment {
    enum transport_kind kind;
    const char *name, *object_path, *stage, *case_name;
    int possible, available, threads, started, requested_cpus;
    int *cpu_ids, *ring_fds, *boundary_fds;
    size_t *ring_sizes, *perf_sizes;
    size_t bytes, page;
    uint64_t period_ns, boundary_offset_ns, began_ns, ended_ns;
    uint64_t busy_start, busy_end, busy_cpu_ns, process_cpu_ns;
    long clock_ticks;
    double duration, elapsed, max_gap_ms;
    bool pause_drain, one_page, accounting_ok;
    bool syscall_boundary, syscall_demand;
    int helpers_started;
    _Atomic uint64_t *consumed_frontier;
    struct cpu_snapshot *cpu_start, *cpu_end;
    struct boundary_helper *helpers;
    struct syscall_boundary_counts *syscall_counts;
    struct bpf_link *syscall_link;
    struct writer *writers;
    struct transport_cpu_result *results;
    struct transport_counts *counts[TRANSPORT_SITES];
    struct bpf_object *object;
    struct ring_buffer *ring;
    struct perf_buffer **perf;
    uint64_t lost, lost_callbacks, bad_records;
    int error, saved_errno;
    bool accepted;
};
static atomic_int ready, helpers_ready, start_writers, stop_writers;
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
static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
static uint64_t process_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts)) { perror("process clock"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
/* /proc/stat user+nice+system+irq+softirq+steal (guest already in user).
 * Includes idle-task hardirq boundary work only with CONFIG_IRQ_TIME_ACCOUNTING.
 * Otherwise it is booked as idle; qualify.py marks the idle CPU gate UNMEASURED.
 * Fail closed on unreadable/vanished participating CPUs; hotplug excluded. */
static int busy_ticks(struct experiment *e, uint64_t *total, struct cpu_snapshot *snapshot)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -errno;
    int rc = transport_cpu_ticks(f, e->cpu_ids, e->available, e->possible, snapshot, total) ? 0 : -EIO;
    fclose(f);
    return rc;
}
static int pin_cpu(int cpu, int possible)
{
    cpu_set_t *mask = CPU_ALLOC((size_t)possible);
    if (!mask) return ENOMEM;
    size_t size = CPU_ALLOC_SIZE((size_t)possible);
    CPU_ZERO_S(size, mask);
    CPU_SET_S(cpu, size, mask);
    int rc = sched_setaffinity(0, size, mask) ? errno : 0;
    CPU_FREE(mask);
    return rc;
}
static void *write_loop(void *arg)
{
    struct writer *w = arg;
    struct experiment *e = w->experiment;
    w->error = pin_cpu(w->cpu, e->possible);
    w->tid = (pid_t)syscall(SYS_gettid);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&start_writers, memory_order_acquire) &&
           !atomic_load_explicit(&stop_writers, memory_order_relaxed)) sched_yield();
    if (w->error) return NULL;
    uint64_t n = 0;
    int next_cpu = 0;
    while (!atomic_load_explicit(&stop_writers, memory_order_relaxed)) {
        if (!(n & 255)) {
            if (now_seconds() >= writer_deadline) break;
            if (!strcmp(e->case_name, "migrate") && e->available > 1 && n) {
                next_cpu = (next_cpu + 1) % e->available;
                w->error = pin_cpu(e->cpu_ids[next_cpu], e->possible);
                if (w->error) break;
                ++w->migrations;
            }
        }
        w->watched = ++n;
    }
    w->iterations = n;
    return NULL;
}

static int thread_cpu_ns(uint64_t *value)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return errno;
    *value = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
    return 0;
}
static void *boundary_loop(void *arg)
{
    struct boundary_helper *h = arg;
    struct experiment *e = h->experiment;
    h->error = pin_cpu(h->cpu, e->possible);
    h->tid = (pid_t)syscall(SYS_gettid);
    if (h->tid < 0 && !h->error) h->error = errno;
    atomic_fetch_add_explicit(&helpers_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&start_writers, memory_order_acquire) &&
           !atomic_load_explicit(&stop_writers, memory_order_relaxed)) sched_yield();
    if (h->error || atomic_load_explicit(&stop_writers, memory_order_relaxed)) return NULL;
    uint64_t began = 0;
    h->error = thread_cpu_ns(&began);
    if (h->error) return NULL;
    uint64_t next = e->began_ns;
    uint64_t end = e->began_ns + (uint64_t)(e->duration * 1e9);
    while (!atomic_load_explicit(&stop_writers, memory_order_relaxed) && now_ns() < end) {
        /* Explicit syscall: never a libc cache or vDSO clock observation. */
        ++h->wakeups;
        uint64_t frontier = atomic_load_explicit(&e->consumed_frontier[h->cpu], memory_order_acquire);
        uint64_t wake = now_ns();
        if (transport_boundary_due(e->syscall_demand, wake, frontier, e->period_ns)) {
            if (syscall(SYS_getppid) < 0) { h->error = errno; break; }
            ++h->syscalls;
        }
        uint64_t current = now_ns();
        /* Skip missed deadlines rather than emitting a catch-up burst. */
        next = transport_next_deadline(next, current, end, e->period_ns);
        struct timespec deadline = { .tv_sec = (time_t)(next / 1000000000),
                                     .tv_nsec = (long)(next % 1000000000) };
        int sleep_rc;
        do {
            sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (sleep_rc == EINTR && !atomic_load_explicit(&stop_writers, memory_order_relaxed));
        if (sleep_rc && sleep_rc != EINTR) h->error = sleep_rc;
        if (h->error) break;
    }
    uint64_t ended = began;
    int rc = thread_cpu_ns(&ended);
    if (rc) h->error = rc;
    else h->cpu_ns = ended - began;
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
static int open_boundary(int cpu, uint64_t period_ns, int program)
{
    struct perf_event_attr attr = {0};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.freq = 0;
    attr.sample_period = period_ns;
    attr.exclude_idle = 0;
    attr.disabled = 1;
    int fd = (int)syscall(__NR_perf_event_open, &attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, program)) {
        int error = errno;
        close(fd); errno = error; return -1;
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
    struct transport_cpu_result *c = &e->results[r.cpu];
    if (!c->participating || !transport_record_valid(&r, c->expected_tid) ||
        (r.site == TRANSPORT_BOUNDARY && !e->period_ns)) {
        ++e->bad_records; return 0;
    }
    transport_observe(c, &r, now_ns());
    atomic_store_explicit(&e->consumed_frontier[r.cpu], c->frontier_ns, memory_order_release);
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
    struct bpf_program *boundary = bpf_object__find_program_by_name(e->object, "on_boundary");
    if (!boundary) return -ENOENT;
    int boundary_rc = bpf_program__set_autoload(boundary, e->period_ns && !e->syscall_boundary);
    if (boundary_rc) return boundary_rc;
    boundary = bpf_object__find_program_by_name(e->object, "on_syscall_boundary");
    if (!boundary) return -ENOENT;
    if ((boundary_rc = bpf_program__set_autoload(boundary, e->syscall_boundary))) return boundary_rc;
    const char *helper_maps[] = {"helper_tgid", "helper_cpus", "syscall_counts"};
    for (size_t i = 0; i < sizeof(helper_maps) / sizeof(helper_maps[0]); ++i) {
        struct bpf_map *helper_map = bpf_object__find_map_by_name(e->object, helper_maps[i]);
        if (!helper_map) return -ENOENT;
        if ((boundary_rc = bpf_map__set_autocreate(helper_map, e->syscall_boundary))) return boundary_rc;
        if (i == 1 && e->syscall_boundary &&
            (boundary_rc = bpf_map__set_max_entries(helper_map, (__u32)e->available))) return boundary_rc;
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
        struct transport_config config = {
            .transport = (unsigned int)e->kind, .boundary_offset_ns = e->boundary_offset_ns
        };
        if (bpf_map__value_size(map) != sizeof(config)) return -EINVAL;
        if ((rc = bpf_map__set_initial_value(map, &config, sizeof(config)))) return rc;
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
static void stop_and_join(struct experiment *e)
{
    atomic_store_explicit(&stop_writers, 1, memory_order_relaxed);
    for (int i = 0; i < e->started; ++i) pthread_join(e->writers[i].thread, NULL);
    e->started = 0;
    for (int i = 0; i < e->helpers_started; ++i) pthread_join(e->helpers[i].thread, NULL);
    e->helpers_started = 0;
}
static int start_syscall_boundary(struct experiment *e)
{
    e->stage = "helper_start";
    int rc;
    int tgid_fd = bpf_object__find_map_fd_by_name(e->object, "helper_tgid");
    int cpus_fd = bpf_object__find_map_fd_by_name(e->object, "helper_cpus");
    if (tgid_fd < 0 || cpus_fd < 0) return -ENOENT;
    uint32_t zero = 0, tgid = (uint32_t)getpid();
    if (bpf_map_update_elem(tgid_fd, &zero, &tgid, BPF_ANY)) return -errno;
    for (int i = 0; i < e->available; ++i) {
        struct boundary_helper *h = &e->helpers[i];
        h->cpu = e->cpu_ids[i]; h->experiment = e;
        rc = pthread_create(&h->thread, NULL, boundary_loop, h);
        if (rc) return -rc;
        ++e->helpers_started;
    }
    while (atomic_load_explicit(&helpers_ready, memory_order_acquire) < e->available) sched_yield();
    for (int i = 0; i < e->available; ++i) {
        struct boundary_helper *h = &e->helpers[i];
        if (h->error) return -h->error;
        uint32_t tid = (uint32_t)h->tid, cpu = (uint32_t)h->cpu;
        if (bpf_map_update_elem(cpus_fd, &tid, &cpu, BPF_ANY)) return -errno;
    }
    struct bpf_program *program = bpf_object__find_program_by_name(e->object, "on_syscall_boundary");
    if (!program) return -ENOENT;
    e->stage = "syscall_boundary_attach";
    e->syscall_link = bpf_program__attach_tracepoint(program, "syscalls", "sys_enter_getppid");
    long error = libbpf_get_error(e->syscall_link);
    if (!e->syscall_link || error) {
        e->syscall_link = NULL;
        return error ? (int)error : -errno;
    }
    return 0;
}
static int run(struct experiment *e)
{
    e->stage = "writers_start";
    for (int i = 0; i < e->threads; ++i) {
        struct writer *w = &e->writers[i];
        w->experiment = e;
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
        if (!strcmp(e->case_name, "hog")) continue;
        if (!strcmp(e->case_name, "migrate")) {
            for (int j = 0; j < e->available; ++j)
                e->results[e->cpu_ids[j]].expected_tid = (uint32_t)w->tid;
        } else e->results[w->cpu].expected_tid = (uint32_t)w->tid;
        e->stage = "breakpoint_attach";
        w->fd = open_breakpoint(w, bpf_program__fd(program));
        if (w->fd < 0) return -errno;
        if (ioctl(w->fd, PERF_EVENT_IOC_ENABLE, 0)) return -errno;
    }
    if (e->syscall_boundary) {
        int rc = start_syscall_boundary(e);
        if (rc) return rc;
    } else if (e->period_ns) {
        program = bpf_object__find_program_by_name(e->object, "on_boundary");
        if (!program) return -ENOENT;
        e->stage = "boundary_attach";
        for (int i = 0; i < e->available; ++i) {
            e->boundary_fds[i] = open_boundary(e->cpu_ids[i], e->period_ns, bpf_program__fd(program));
            if (e->boundary_fds[i] < 0) return -errno;
        }
    }
    e->stage = "measurement";
    int rc = busy_ticks(e, &e->busy_start, e->cpu_start);
    if (rc) return rc;
    uint64_t process_start = process_ns();
    e->began_ns = now_ns();
    for (int i = 0; i < e->available; ++i) {
        struct transport_cpu_result *c = &e->results[e->cpu_ids[i]];
        c->boundary_seen_ns = c->frontier_seen_ns = c->measurement_start_ns = e->began_ns;
    }
    writer_deadline = now_seconds() + e->duration;
    /* Start the boundary before permitting user stores; include startup
     * and final silence in the per-CPU consumed-boundary deadline check. */
    for (int i = 0; i < e->available; ++i)
        if (e->period_ns && !e->syscall_boundary &&
            ioctl(e->boundary_fds[i], PERF_EVENT_IOC_ENABLE, 0)) return -errno;
    atomic_store_explicit(&start_writers, 1, memory_order_release);
    double last = now_seconds();
    while (!atomic_load_explicit(&interrupted, memory_order_relaxed) &&
           now_ns() - e->began_ns < (uint64_t)(e->duration * 1e9)) {
        if (e->pause_drain) {
            struct timespec pause = { .tv_nsec = 10000000 };
            nanosleep(&pause, NULL);
        } else {
            rc = drain(e, true);
            if (rc < 0) return rc;
        }
        double current = now_seconds();
        double gap = (current - last) * 1000;
        if (gap > e->max_gap_ms) e->max_gap_ms = gap;
        last = current;
    }
    stop_and_join(e);
    e->stage = "quiescence";
    for (int i = 0; i < e->threads; ++i) {
        if (e->writers[i].error) return -e->writers[i].error;
        if (e->writers[i].fd >= 0 && ioctl(e->writers[i].fd, PERF_EVENT_IOC_DISABLE, 0)) return -errno;
    }
    /* Disabling a local event under the kernel's event-function call also
     * waits for its running callback on the target CPU to complete. Close
     * before accounting, so an unfinished failure path cannot be mistaken
     * for quiescence. Joined DATA writers have already returned from #DB. */
    for (int i = 0; i < e->available; ++i) if (e->period_ns && !e->syscall_boundary) {
        if (ioctl(e->boundary_fds[i], PERF_EVENT_IOC_DISABLE, 0)) return -errno;
        close(e->boundary_fds[i]); e->boundary_fds[i] = -1;
    }
    if (e->syscall_boundary) {
        for (int i = 0; i < e->available; ++i)
            if (e->helpers[i].error) return -e->helpers[i].error;
        bpf_link__destroy(e->syscall_link); e->syscall_link = NULL;
        int fd = bpf_object__find_map_fd_by_name(e->object, "syscall_counts");
        uint32_t zero = 0;
        if (fd < 0) return fd;
        if (bpf_map_lookup_elem(fd, &zero, e->syscall_counts)) return -errno;
    }
    do { rc = drain(e, false); } while (rc > 0);
    if (rc < 0) return rc;
    double final_gap_ms = (now_seconds() - last) * 1000;
    if (final_gap_ms > e->max_gap_ms) e->max_gap_ms = final_gap_ms;
    e->ended_ns = now_ns();
    e->elapsed = (double)(e->ended_ns - e->began_ns) / 1e9;
    e->process_cpu_ns = process_ns() - process_start;
    if ((rc = busy_ticks(e, &e->busy_end, e->cpu_end))) return rc;
    if (e->busy_end < e->busy_start) return -ERANGE;
    e->busy_cpu_ns = (uint64_t)((long double)(e->busy_end - e->busy_start) * 1e9L / e->clock_ticks);
    for (int i = 0; i < e->available; ++i) {
        struct transport_cpu_result *c = &e->results[e->cpu_ids[i]];
        if (e->period_ns) transport_boundary_gap(c, e->ended_ns);
        transport_frontier_end(c, e->ended_ns);
    }
    int fd = bpf_object__find_map_fd_by_name(e->object, "counts");
    if (fd < 0) return fd;
    e->accounting_ok = true;
    for (uint32_t site = 0; site < TRANSPORT_SITES; ++site) {
        if (bpf_map_lookup_elem(fd, &site, e->counts[site])) return -errno;
        for (int cpu = 0; cpu < e->possible; ++cpu) {
            uint64_t loss = 0, gaps = 0;
            if (!transport_reconcile(&e->results[cpu].site[site], &e->counts[site][cpu], &loss, &gaps))
                e->accounting_ok = false;
            if (!e->results[cpu].participating && e->counts[site][cpu].attempts)
                e->accounting_ok = false;
        }
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
    uint64_t hits = 0, migrations = 0, attempts = 0, delivered = 0, failures = 0;
    uint64_t seals = 0, regressions = 0, seq_gaps = 0, reorders = 0;
    uint64_t boundary_fires = 0, boundary_outputs = 0, boundary_gap = 0, wrong_cpu = 0;
    for (int cpu = 0; cpu < e->possible; ++cpu) {
        struct transport_cpu_result *c = &e->results[cpu];
        for (int site = 0; site < TRANSPORT_SITES; ++site) {
            struct transport_counts *n = &e->counts[site][cpu];
            uint64_t loss = 0, gaps = 0;
            (void)transport_reconcile(&c->site[site], n, &loss, &gaps);
            attempts += n->attempts; delivered += c->site[site].delivered;
            failures += n->failures; seq_gaps += gaps; reorders += c->site[site].reorders;
        }
        boundary_fires += e->syscall_boundary ? e->syscall_counts[cpu].fires :
            e->counts[TRANSPORT_BOUNDARY][cpu].attempts;
        boundary_outputs += e->syscall_boundary ? e->syscall_counts[cpu].outputs :
            e->counts[TRANSPORT_BOUNDARY][cpu].successes;
        wrong_cpu += e->syscall_counts[cpu].wrong_cpu;
        seals += c->seal_violations; regressions += c->regressions;
        if (c->max_boundary_gap_ns > boundary_gap) boundary_gap = c->max_boundary_gap_ns;
    }
    for (int i = 0; i < e->threads; ++i) {
        if (strcmp(e->case_name, "hog")) hits += e->writers[i].iterations;
        migrations += e->writers[i].migrations;
    }
    printf("{\"transport\":"); json_string(e->name);
    printf(",\"case\":"); json_string(e->case_name);
    printf(",\"kernel\":"); json_string(kernel);
    printf(",\"boundary_source\":");
    json_string(e->syscall_demand ? "syscall_demand" : e->syscall_boundary ? "syscall" : e->period_ns ? "perf" : "none");
    printf(",\"wrong_cpu_events\":%" PRIu64, wrong_cpu);
    printf(",\"cpus\":%d,\"possible_cpus\":%d,\"threads\":%d,\"duration\":%.6f,"
           "\"elapsed\":%.6f,\"buffer_bytes_total\":%zu,\"period_ns\":%" PRIu64
           ",\"boundary_offset_ns\":%" PRIu64 ",\"pause_drain\":%s,\"hits\":%" PRIu64
           ",\"migrations\":%" PRIu64 ",\"attempts\":%" PRIu64
           ",\"delivered\":%" PRIu64 ",\"helper_failures\":%" PRIu64
           ",\"lost_notifications\":%" PRIu64 ",\"lost_callbacks\":%" PRIu64
           ",\"seq_gaps\":%" PRIu64 ",\"seq_reorders\":%" PRIu64
           ",\"seal_violations\":%" PRIu64 ",\"timestamp_regressions\":%" PRIu64
           ",\"boundary_fires\":%" PRIu64 ",\"boundary_outputs\":%" PRIu64
           ",\"max_boundary_gap_ns\":%" PRIu64 ",\"bad_records\":%" PRIu64
           ",\"max_drain_gap_ms\":%.6f,\"participating_busy_cpu_ns\":%" PRIu64
           ",\"process_cpu_ns\":%" PRIu64 ",\"participating_busy_ticks_start\":%" PRIu64
           ",\"participating_busy_ticks_end\":%" PRIu64 ",\"clock_ticks\":%ld,"
           "\"accounting_ok\":%s,\"accepted\":%s,\"stage\":",
           e->available, e->possible, e->threads, e->duration, e->elapsed, e->bytes,
           e->period_ns, e->boundary_offset_ns, e->pause_drain ? "true" : "false",
           hits, migrations, attempts, delivered, failures, e->lost, e->lost_callbacks,
           seq_gaps, reorders, seals, regressions, boundary_fires, boundary_outputs,
           boundary_gap, e->bad_records, e->max_gap_ms, e->busy_cpu_ns, e->process_cpu_ns,
           e->busy_start, e->busy_end, e->clock_ticks,
           e->accounting_ok ? "true" : "false", e->accepted ? "true" : "false");
    json_string(e->stage);
    printf(",\"libbpf_error\":%d,\"errno\":%d,\"error\":", e->error, e->saved_errno);
    json_string(e->error ? strerror(-e->error) : "");
    printf(",\"proc_stat_whole_box_ticks\":");
    transport_report_cpu_ticks(stdout, &e->cpu_start[e->possible], &e->cpu_end[e->possible]);
    struct cpu_snapshot sum_start = {0}, sum_end = {0};
    for (int i = 0; i < e->available; ++i)
        for (int field = 0; field < CPU_FIELDS; ++field) {
            sum_start.ticks[field] += e->cpu_start[e->cpu_ids[i]].ticks[field];
            sum_end.ticks[field] += e->cpu_end[e->cpu_ids[i]].ticks[field];
        }
    printf(",\"proc_stat_participating_ticks\":");
    transport_report_cpu_ticks(stdout, &sum_start, &sum_end);
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
    for (int i = 0; i < e->available; ++i) {
        int cpu = e->cpu_ids[i];
        struct transport_cpu_result *c = &e->results[cpu];
        printf("%s{\"cpu\":%d,\"seal_violations\":%" PRIu64
               ",\"timestamp_regressions\":%" PRIu64
               ",\"max_boundary_gap_ns\":%" PRIu64
               ",\"max_boundary_stamp_gap_ns\":%" PRIu64
               ",\"frontier_ns\":%" PRIu64 ",\"frontier_seen_ns\":%" PRIu64
               ",\"max_frontier_gap_ns\":%" PRIu64 ",\"max_frontier_stamp_gap_ns\":%" PRIu64
               ",\"syscall_fires\":%" PRIu64 ",\"syscall_outputs\":%" PRIu64
               ",\"wrong_cpu_events\":%" PRIu64 ",\"helper_tid\":%d"
               ",\"helper_cpu_ns\":%" PRIu64 ",\"helper_wakeups\":%" PRIu64
               ",\"helper_syscalls\":%" PRIu64 ",\"boundary_expected\":%" PRIu64
               ",\"hog_tid\":%d,\"hog_iterations\":%" PRIu64,
               i ? "," : "", cpu, c->seal_violations, c->regressions,
               c->max_boundary_gap_ns, c->max_boundary_stamp_gap_ns,
               c->frontier_ns, c->frontier_seen_ns, c->max_frontier_gap_ns, c->max_frontier_stamp_gap_ns,
               e->syscall_counts[cpu].fires, e->syscall_counts[cpu].outputs,
               e->syscall_counts[cpu].wrong_cpu, e->helpers[i].tid, e->helpers[i].cpu_ns,
               e->helpers[i].wakeups, e->helpers[i].syscalls,
               e->period_ns ? ((uint64_t)(e->duration * 1e9) - 1) / e->period_ns + 1 : 0,
               !strcmp(e->case_name, "hog") ? e->writers[i].tid : 0,
               !strcmp(e->case_name, "hog") ? e->writers[i].iterations : 0);
        printf(",\"proc_stat_ticks\":");
        transport_report_cpu_ticks(stdout, &e->cpu_start[cpu], &e->cpu_end[cpu]);
        printf(",\"sites\":[");
        for (int site = 0; site < TRANSPORT_SITES; ++site) {
            struct transport_counts *v = &e->counts[site][cpu];
            struct transport_site_result *r = &c->site[site];
            uint64_t loss = 0, gaps = 0;
            bool reconciled = transport_reconcile(r, v, &loss, &gaps);
            printf("%s{\"site\":\"%s\",\"produced\":%" PRIu64
                   ",\"successes\":%" PRIu64 ",\"failures\":%" PRIu64
                   ",\"delivered\":%" PRIu64 ",\"loss\":%" PRIu64
                   ",\"seq_gaps\":%" PRIu64 ",\"seq_reorders\":%" PRIu64
                   ",\"EAGAIN\":%" PRIu64 ",\"ENOSPC\":%" PRIu64
                   ",\"other\":%" PRIu64 ",\"reserve_null\":%" PRIu64
                   ",\"reconciled\":%s}", site ? "," : "", site ? "BOUNDARY" : "DATA",
                   v->attempts, v->successes, v->failures, r->delivered, loss, gaps,
                   r->reorders, v->eagain, v->enospc, v->other, v->reserve_null,
                   reconciled ? "true" : "false");
        }
        printf("]}");
    }
    puts("]}");
}
static void cleanup(struct experiment *e)
{
    stop_and_join(e);
    if (e->syscall_link) bpf_link__destroy(e->syscall_link);
    if (e->writers)
        for (int i = 0; i < e->threads; ++i) if (e->writers[i].fd >= 0) close(e->writers[i].fd);
    if (e->boundary_fds)
        for (int i = 0; i < e->available; ++i) if (e->boundary_fds[i] >= 0) close(e->boundary_fds[i]);
    if (e->ring) ring_buffer__free(e->ring);
    if (e->perf)
        for (int i = 0; i < e->available; ++i) if (e->perf[i]) perf_buffer__free(e->perf[i]);
    if (e->object) bpf_object__close(e->object);
    if (e->ring_fds)
        for (int i = 0; i < e->possible; ++i) if (e->ring_fds[i] >= 0) close(e->ring_fds[i]);
    free(e->cpu_ids); free(e->writers); free(e->results);
    free(e->helpers); free(e->syscall_counts);
    free(e->consumed_frontier); free(e->cpu_start); free(e->cpu_end);
    for (int site = 0; site < TRANSPORT_SITES; ++site) free(e->counts[site]);
    free(e->ring_fds); free(e->ring_sizes); free(e->perf_sizes); free(e->perf); free(e->boundary_fds);
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
    fprintf(stderr, "usage: %s --transport shared|percpu-ring|perf [--cpus N] "
            "[--case idle|one|all|migrate|hog] [--duration SECONDS] [--bytes TOTAL] "
            "[--period-ns N] [--syscall-boundary|--syscall-demand] [--pause-drain] [--mutant-boundary] "
            "[--object transport.bpf.o]\n", program);
}
int main(int argc, char **argv)
{
    struct experiment e = {
        .name = "shared", .object_path = "transport.bpf.o", .stage = "setup",
        .case_name = "all", .bytes = 64 * 1024 * 1024, .duration = 30
    };
    bool boundary_requested = false;
    static const struct option options[] = {
        {"transport", required_argument, NULL, 't'}, {"threads", required_argument, NULL, 'n'},
        {"cpus", required_argument, NULL, 'n'}, {"case", required_argument, NULL, 'c'},
        {"duration", required_argument, NULL, 'd'}, {"bytes", required_argument, NULL, 'b'},
        {"object", required_argument, NULL, 'o'}, {"period-ns", required_argument, NULL, 'p'},
        {"boundary", no_argument, NULL, 'B'}, {"pause-drain", no_argument, NULL, 'P'},
        {"syscall-boundary", no_argument, NULL, 'S'},
        {"syscall-demand", no_argument, NULL, 'D'},
        {"mutant-boundary", no_argument, NULL, 'M'}, {"one-page", no_argument, NULL, 'O'},
        {"help", no_argument, NULL, 'h'}, {0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "t:n:c:d:b:o:p:BSDPMOh", options, NULL)) != -1) {
        size_t n;
        switch (opt) {
        case 't':
            if (!strcmp(optarg, "shared")) e.kind = TRANSPORT_SHARED;
            else if (!strcmp(optarg, "percpu-ring")) e.kind = TRANSPORT_PERCPU_RING;
            else if (!strcmp(optarg, "perf")) e.kind = TRANSPORT_PERF;
            else goto invalid;
            e.name = optarg;
            break;
        case 'n': if (parse_size(optarg, &n) || n > INT_MAX) goto invalid; e.requested_cpus = (int)n; break;
        case 'c':
            if (strcmp(optarg, "idle") && strcmp(optarg, "one") &&
                strcmp(optarg, "all") && strcmp(optarg, "migrate") && strcmp(optarg, "hog")) goto invalid;
            e.case_name = optarg; break;
        case 'd': if (parse_size(optarg, &n) || n > 86400) goto invalid; e.duration = (double)n; break;
        case 'b': if (parse_size(optarg, &e.bytes)) goto invalid; break;
        case 'p': if (parse_size(optarg, &n)) goto invalid; e.period_ns = n; boundary_requested = true; break;
        case 'B': boundary_requested = true; break;
        case 'D': e.syscall_demand = true; e.syscall_boundary = true; boundary_requested = true; break;
        case 'S': e.syscall_boundary = true; boundary_requested = true; break;
        case 'P': e.pause_drain = true; break;
        case 'O': e.one_page = true; break;
        case 'M': e.boundary_offset_ns = 1000000; boundary_requested = true; break;
        case 'o': e.object_path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default: goto invalid;
        }
    }
    if (optind != argc || ((boundary_requested || e.one_page) && e.kind != TRANSPORT_PERF)) goto invalid;
    if (boundary_requested && !e.period_ns) e.period_ns = 10000000;
    e.possible = libbpf_num_possible_cpus();
    long page = sysconf(_SC_PAGESIZE);
    e.clock_ticks = sysconf(_SC_CLK_TCK);
    if (e.possible <= 0 || page <= 0 || e.clock_ticks <= 0) {
        fprintf(stderr, "cannot discover CPUs/page size/clock ticks\n"); return 1;
    }
    e.page = (size_t)page;
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
    if (!e.available || e.requested_cpus > e.available) { CPU_FREE(mask); cleanup(&e); goto invalid; }
    if (e.requested_cpus) e.available = e.requested_cpus;
    /* Keep reader, writers and hardirq accounting on exactly these CPUs. */
    CPU_ZERO_S(mask_size, mask);
    for (int i = 0; i < e.available; ++i) CPU_SET_S(e.cpu_ids[i], mask_size, mask);
    if (sched_setaffinity(0, mask_size, mask)) {
        perror("sched_setaffinity"); CPU_FREE(mask); cleanup(&e); return 1;
    }
    CPU_FREE(mask);
    e.threads = !strcmp(e.case_name, "idle") ? 0 : (!strcmp(e.case_name, "all") || !strcmp(e.case_name, "hog")) ? e.available : 1;
    size_t writers_size = (size_t)(e.threads ? e.threads : 1) * sizeof(*e.writers);
    e.writers = aligned_alloc(64, writers_size);
    e.results = calloc((size_t)e.possible, sizeof(*e.results));
    e.consumed_frontier = calloc((size_t)e.possible, sizeof(*e.consumed_frontier));
    e.cpu_start = calloc((size_t)e.possible + 1, sizeof(*e.cpu_start));
    e.cpu_end = calloc((size_t)e.possible + 1, sizeof(*e.cpu_end));
    e.helpers = calloc((size_t)e.available, sizeof(*e.helpers));
    e.syscall_counts = calloc((size_t)e.possible, sizeof(*e.syscall_counts));
    for (int site = 0; site < TRANSPORT_SITES; ++site)
        e.counts[site] = calloc((size_t)e.possible, sizeof(*e.counts[site]));
    e.ring_fds = malloc((size_t)e.possible * sizeof(*e.ring_fds));
    e.boundary_fds = malloc((size_t)e.available * sizeof(*e.boundary_fds));
    e.ring_sizes = calloc((size_t)e.possible, sizeof(*e.ring_sizes));
    e.perf_sizes = calloc((size_t)e.available, sizeof(*e.perf_sizes));
    e.perf = calloc((size_t)e.available, sizeof(*e.perf));
    if (e.ring_fds) for (int i = 0; i < e.possible; ++i) e.ring_fds[i] = -1;
    if (e.boundary_fds) for (int i = 0; i < e.available; ++i) e.boundary_fds[i] = -1;
    if (e.writers) {
        memset(e.writers, 0, writers_size);
        for (int i = 0; i < e.threads; ++i) { e.writers[i].fd = -1; e.writers[i].cpu = e.cpu_ids[i]; }
    }
    if (!e.writers || !e.results || !e.helpers || !e.syscall_counts ||
        !e.consumed_frontier || !e.cpu_start || !e.cpu_end ||
        !e.counts[0] || !e.counts[1] || !e.ring_fds ||
        !e.ring_sizes || !e.perf_sizes || !e.perf || !e.boundary_fds) {
        fprintf(stderr, "allocation failure\n"); cleanup(&e); return 1;
    }
    for (int cpu = 0; cpu < e.possible; ++cpu) atomic_init(&e.consumed_frontier[cpu], 0);
    for (int i = 0; i < e.available; ++i) {
        e.results[e.cpu_ids[i]].participating = true;
        e.results[e.cpu_ids[i]].qualified_perf = e.kind == TRANSPORT_PERF;
    }
    /* The single-page control must also work on hosts with many possible
     * CPUs; never allocate or budget unused ring maps in a perf run. */
    if (e.one_page) e.bytes = (size_t)e.available * e.page;
    int budget_rc = e.kind == TRANSPORT_PERCPU_RING ?
        split_budget(e.bytes, e.page, e.possible, e.ring_sizes) :
        e.kind == TRANSPORT_PERF ? split_budget(e.bytes, e.page, e.available, e.perf_sizes) : 0;
    if (e.one_page) {
        for (int i = 0; i < e.available; ++i) e.perf_sizes[i] = e.page;
        budget_rc = 0;
    }
    if (budget_rc) {
        fprintf(stderr, "budget must provide one page per buffer\n"); cleanup(&e); return 2;
    }
    struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &limit)) perror("setrlimit MEMLOCK (continuing)");
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    errno = 0;
    e.error = configure_object(&e);
    if (!e.error) e.error = run(&e);
    e.saved_errno = errno;
    if (e.error) fprintf(stderr, "%s: libbpf/operation error=%d errno=%d (%s)\n",
                         e.stage, e.error, e.saved_errno, strerror(-e.error));
    stop_and_join(&e);
    report(&e);
    int status = e.error ? (e.accepted ? 1 : 3) : 0;
    cleanup(&e);
    return status;
invalid:
    usage(argv[0]); return 2;
}
