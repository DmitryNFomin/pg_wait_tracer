/* Linux binding for the portable loss-vector reader. Kept separate from
 * map_reader.o: BPF-free accumulator consumers do not link capture helpers. */
#include "capture_counters.h"
#include "daemon.h"
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

/* Read one cumulative BPF failure slot across every possible CPU. */
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *d, uint32_t slot)
{
    if (!d->skel || slot >= PGWT_BPF_FAIL_MAX)
        return (struct pgwt_counter_value){0, false};
    return pgwt_read_percpu_counter(bpf_map__fd(d->skel->maps.fail_counters),
                                   slot, libbpf_num_possible_cpus(),
                                   bpf_map_lookup_elem);
}

struct pgwt_counter_value pgwt_read_daemon_ring_site(struct pgwt_daemon *d, uint32_t slot)
{
    if (!d->skel || slot >= PGWT_RING_FAIL_SITE_MAX)
        return (struct pgwt_counter_value){0, false};
    return pgwt_read_percpu_counter(bpf_map__fd(d->skel->maps.ringbuf_site_drops),
                                   slot, libbpf_num_possible_cpus(), bpf_map_lookup_elem);
}

struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *d)
{
    struct pgwt_counter_sources s = {
        .fail_fd = d->skel ? bpf_map__fd(d->skel->maps.fail_counters) : -1,
        .site_fd = d->skel ? bpf_map__fd(d->skel->maps.ringbuf_site_drops) : -1,
        .event_ring_fd = d->skel ? bpf_map__fd(d->skel->maps.ringbuf_drops) : -1,
        .ncpu = libbpf_num_possible_cpus(),
        .lifecycle_init_lost_total = d->counters.lifecycle_init_lost_total,
        .init_state_read_fail_total = d->counters.init_state_read_fail_total,
        .state_map_full_total = d->counters.state_map_full_total,
        .writer_push_fail_total = d->counters.writer_push_fail_total,
    };
    return pgwt_read_counter_vector(&s, bpf_map_lookup_elem);
}

