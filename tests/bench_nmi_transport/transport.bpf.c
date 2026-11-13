/* No daemon, CO-RE, vmlinux or PostgreSQL dependency. Refs #871. */
#include <linux/bpf.h>
#include <linux/errno.h>
#include <bpf/bpf_helpers.h>
#include "transport.h"

const volatile struct transport_config config = { .transport = TRANSPORT_SHARED };
struct ring_map {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096);
};
struct ring_map shared_ring SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY_OF_MAPS);
    __uint(max_entries, 1);
    __type(key, __u32);
    __array(values, struct ring_map);
} cpu_rings SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} perf_events SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, TRANSPORT_SITES);
    __type(key, __u32);
    __type(value, struct transport_counts);
} counts SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} helper_tgid SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32); /* registered helper TID */
    __type(value, __u32); /* exactly one expected CPU */
} helper_cpus SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct syscall_boundary_counts);
} syscall_counts SEC(".maps");

static __always_inline void count_result(struct transport_counts *c, long rc)
{
    if (!rc) __sync_fetch_and_add(&c->successes, 1);
    else {
        __sync_fetch_and_add(&c->failures, 1);
        if (rc == -EAGAIN) __sync_fetch_and_add(&c->eagain, 1);
        else if (rc == -ENOSPC) __sync_fetch_and_add(&c->enospc, 1);
        else __sync_fetch_and_add(&c->other, 1);
    }
}

SEC("perf_event")
int on_write(void *ctx)
{
    __u32 site = TRANSPORT_DATA, cpu = bpf_get_smp_processor_id();
    struct transport_counts *c = bpf_map_lookup_elem(&counts, &site);
    if (!c) return 0;
    /* Permitted producer: only a userspace write can trigger this program.
     * One writer per CPU, or one migrating writer. No same-site nesting.
     * Boundary uses a DIFFERENT counter slot; see QUALIFICATION.md for the
     * kernel-dependent no-overlap argument (not a qualification claim). */
    __sync_fetch_and_add(&c->attempts, 1);
    struct transport_record record = {
        .cpu = cpu, .site = TRANSPORT_DATA,
        .tid = (__u32)bpf_get_current_pid_tgid(), .seq = c->attempts
    };
    long rc;
    if (config.transport == TRANSPORT_SHARED) {
        record.ts = bpf_ktime_get_ns();
        rc = bpf_ringbuf_output(&shared_ring, &record, sizeof(record), BPF_RB_NO_WAKEUP);
    } else if (config.transport == TRANSPORT_PERCPU_RING) {
        void *ring = bpf_map_lookup_elem(&cpu_rings, &cpu);
        if (!ring) { count_result(c, -ENOENT); return 0; }
        struct transport_record *slot = bpf_ringbuf_reserve(ring, sizeof(record), 0);
        if (!slot) {
            __sync_fetch_and_add(&c->reserve_null, 1);
            count_result(c, -ENOBUFS); /* NULL has no errno: report other. */
            return 0;
        }
        record.ts = bpf_ktime_get_ns();
        __builtin_memcpy(slot, &record, sizeof(record));
        bpf_ringbuf_submit(slot, BPF_RB_NO_WAKEUP);
        count_result(c, 0);
        return 0;
    } else {
        record.ts = bpf_ktime_get_ns();
        rc = bpf_perf_event_output(ctx, &perf_events, BPF_F_CURRENT_CPU,
                                   &record, sizeof(record));
    }
    count_result(c, rc);
    return 0;
}

/* Attached system-wide (pid=-1, cpu=i) to a CPU-clock software event on
 * EVERY participating online CPU, including CPUs without a DATA writer.
 * This hrtimer producer must be non-NMI and cannot overlap the permitted
 * DATA producer's timestamp-to-output section on the qualified kernels. */
SEC("perf_event")
int on_boundary(void *ctx)
{
    __u32 site = TRANSPORT_BOUNDARY;
    struct transport_counts *c = bpf_map_lookup_elem(&counts, &site);
    if (!c) return 0;
    __sync_fetch_and_add(&c->attempts, 1);
    struct transport_record record = {
        .ts = bpf_ktime_get_ns() + config.boundary_offset_ns,
        .cpu = bpf_get_smp_processor_id(), .site = TRANSPORT_BOUNDARY,
        .seq = c->attempts
    };
    long rc = bpf_perf_event_output(ctx, &perf_events, BPF_F_CURRENT_CPU,
                                    &record, sizeof(record));
    count_result(c, rc);
    return 0;
}

/* Task-context syscall tracepoint: no CPU_CLOCK sampling or perf throttle.
 * Only registered helpers in this process can produce a boundary. The
 * userspace write watchpoint cannot fire inside this kernel path. */
SEC("tracepoint/syscalls/sys_enter_getppid")
int on_syscall_boundary(void *ctx)
{
    __u64 id = bpf_get_current_pid_tgid();
    __u32 zero = 0, tid = (__u32)id;
    __u32 *tgid = bpf_map_lookup_elem(&helper_tgid, &zero);
    if (!tgid || *tgid != (__u32)(id >> 32)) return 0;
    __u32 *expected_cpu = bpf_map_lookup_elem(&helper_cpus, &tid);
    if (!expected_cpu) return 0;
    struct syscall_boundary_counts *s = bpf_map_lookup_elem(&syscall_counts, &zero);
    if (!s) return 0;
    __sync_fetch_and_add(&s->fires, 1);
    __u32 cpu = bpf_get_smp_processor_id();
    if (cpu != *expected_cpu) {
        __sync_fetch_and_add(&s->wrong_cpu, 1);
        return 0;
    }
    __u32 site = TRANSPORT_BOUNDARY;
    struct transport_counts *c = bpf_map_lookup_elem(&counts, &site);
    if (!c) return 0;
    __sync_fetch_and_add(&c->attempts, 1);
    struct transport_record record = {
        .ts = bpf_ktime_get_ns() + config.boundary_offset_ns,
        .cpu = cpu, .site = TRANSPORT_BOUNDARY, .seq = c->attempts
    };
    long rc = bpf_perf_event_output(ctx, &perf_events, BPF_F_CURRENT_CPU,
                                    &record, sizeof(record));
    if (!rc) __sync_fetch_and_add(&s->outputs, 1);
    count_result(c, rc);
    return 0;
}
char LICENSE[] SEC("license") = "GPL";
