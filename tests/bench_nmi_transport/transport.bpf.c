/* No daemon, CO-RE, vmlinux or PostgreSQL dependency. Refs #871. */
#include <linux/bpf.h>
#include <linux/errno.h>
#include <bpf/bpf_helpers.h>
#include "transport.h"

const volatile unsigned int selected_transport = TRANSPORT_SHARED;

struct ring_map {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096);
};
struct ring_map shared_ring SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY_OF_MAPS);
    __uint(max_entries, 1); /* resized to possible CPUs before loading */
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
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct transport_counts);
} counts SEC(".maps");

SEC("perf_event")
int on_write(void *ctx)
{
    __u32 zero = 0, cpu = bpf_get_smp_processor_id();
    struct transport_counts *c = bpf_map_lookup_elem(&counts, &zero);
    if (!c) return 0;
    /* Only this user-mode watchpoint program writes these counters. There
     * is one pinned thread and one breakpoint per participating CPU; kernel
     * execution cannot write the watched userspace word. Thus no same-CPU
     * nested writer or migration/reset can interrupt this increment. */
    c->attempts++;
    struct transport_record record = {
        .cpu = cpu,
        .tid = (__u32)bpf_get_current_pid_tgid(), .seq = c->attempts
    };
    long rc;
    if (selected_transport == TRANSPORT_SHARED) {
        record.ts = bpf_ktime_get_ns();
        rc = bpf_ringbuf_output(&shared_ring, &record, sizeof(record), BPF_RB_NO_WAKEUP);
    } else if (selected_transport == TRANSPORT_PERCPU_RING) {
        void *ring = bpf_map_lookup_elem(&cpu_rings, &cpu);
        if (!ring) { c->other++; return 0; }
        struct transport_record *slot = bpf_ringbuf_reserve(ring, sizeof(record), 0);
        if (!slot) {
            /* reserve exposes NULL, NOT errno: do not invent EAGAIN/ENOSPC. */
            c->reserve_null++;
            c->other++;
            return 0;
        }
        /* Per-CPU ring arm also exercises the reserve-before-time rule. */
        record.ts = bpf_ktime_get_ns();
        __builtin_memcpy(slot, &record, sizeof(record));
        bpf_ringbuf_submit(slot, BPF_RB_NO_WAKEUP);
        return 0;
    } else {
        record.ts = bpf_ktime_get_ns();
        rc = bpf_perf_event_output(ctx, &perf_events, BPF_F_CURRENT_CPU,
                                   &record, sizeof(record));
    }
    if (rc == -EAGAIN) c->eagain++;
    else if (rc == -ENOSPC) c->enospc++;
    else if (rc) c->other++;
    return 0;
}
char LICENSE[] SEC("license") = "GPL";
